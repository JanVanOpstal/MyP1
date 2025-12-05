#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "wifi_config_server.h"

#define WIFI_AP_SSID "ESP32_Config"
#define WIFI_AP_PASS "12345678"

#define WIFI_CONNECT_TIMEOUT_MS 10000

static const char *TAG = "main";
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// Load SPIFFS
void init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };
    esp_vfs_spiffs_register(&conf);
}

// Save credentials to NVS
void save_wifi_credentials(const char *ssid, const char *password) {
    nvs_handle_t nvs;
    nvs_open("wifi_creds", NVS_READWRITE, &nvs);
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "password", password);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// Load credentials from NVS
bool load_wifi_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi_creds", NVS_READONLY, &nvs);
    if (err != ESP_OK) return false;
    size_t s = ssid_len, p = pass_len;
    if (nvs_get_str(nvs, "ssid", ssid, &s) != ESP_OK ||
        nvs_get_str(nvs, "password", password, &p) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    nvs_close(nvs);
    return true;
}

// WiFi AP setup
void wifi_init_softap(void) {
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .password = WIFI_AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(WIFI_AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "WiFi AP started. SSID:%s password:%s", WIFI_AP_SSID, WIFI_AP_PASS);
}

// WiFi STA setup
void wifi_connect_sta(const char *ssid, const char *password) {
    esp_netif_create_default_wifi_sta();

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting to WiFi SSID:%s", ssid);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool try_connect_sta(const char *ssid, const char *password, int timeout_ms) {
    wifi_event_group = xEventGroupCreate();
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_got_ip);

    wifi_connect_sta(ssid, password);

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    vEventGroupDelete(wifi_event_group);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS Flash init failed: %s", esp_err_to_name(ret));
    }
    esp_netif_init();
    init_spiffs();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    char ssid[32] = {0}, password[64] = {0};
    if (load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Loaded WiFi credentials from NVS. Connecting...");
        if (try_connect_sta(ssid, password, WIFI_CONNECT_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "Connected to WiFi successfully.");
            // Continue normal operation as STA
            wifi_config_server_start();
            ESP_LOGI(TAG, "Started WiFi Config Server in STA mode.");
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            return;
        } else {
            ESP_LOGW(TAG, "Failed to connect as STA, falling back to AP mode.");
        }
    }

    wifi_init_softap();
    wifi_config_server_start();
    ESP_LOGI(TAG, "Started WiFi Config Server in AP mode.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (wifi_config_server_got_credentials(ssid, password)) {
            ESP_LOGI(TAG, "Got credentials from web. Saving to NVS and rebooting...");
            save_wifi_credentials(ssid, password);
            esp_restart();
        }
    }
}