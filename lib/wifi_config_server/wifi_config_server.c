#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "wifi_config_server.h"
#include <sys/param.h>

static const char *TAG = "wifi_cfg_srv";
static char saved_ssid[32] = {0};
static char saved_password[64] = {0};
static bool credentials_received = false;

static esp_err_t root_get_handler(httpd_req_t *req) {
    FILE *file = fopen("/spiffs/index.html", "r");
    if (!file) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char line[128];
    while (fgets(line, sizeof(line), file)) {
        httpd_resp_send_chunk(req, line, strlen(line));
    }
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0); // End response
    return ESP_OK;
}

static esp_err_t wifi_get_handler(httpd_req_t *req) {
    // Option 1: Serve the form directly
    FILE *file = fopen("/spiffs/wifi.html", "r");
    if (!file) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    char line[128];
    while (fgets(line, sizeof(line), file)) {
        httpd_resp_send_chunk(req, line, strlen(line));
    }
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret, remaining = req->content_len;

    char ssid[32] = {0}, password[64] = {0};

    while (remaining > 0) {
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf) - 1))) <= 0) {
            return ESP_FAIL;
        }
        buf[ret] = 0;
        char *ssid_ptr = strstr(buf, "ssid=");
        char *pass_ptr = strstr(buf, "password=");
        if (ssid_ptr) sscanf(ssid_ptr, "ssid=%31[^&]", ssid);
        if (pass_ptr) sscanf(pass_ptr, "password=%63[^&]", password);
        remaining -= ret;
    }

    ESP_LOGI(TAG, "Received SSID: %s, Password: %s", ssid, password);

    strncpy(saved_ssid, ssid, sizeof(saved_ssid) - 1);
    strncpy(saved_password, password, sizeof(saved_password) - 1);
    credentials_received = true;

    httpd_resp_sendstr(req, "WiFi credentials received. Device will connect.");
    return ESP_OK;
}

void wifi_config_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t wifi_get = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_get);

    httpd_uri_t wifi = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi);

    ESP_LOGI(TAG, "Web server started");
}

bool wifi_config_server_got_credentials(char *ssid, char *password) {
    if (credentials_received) {
        strcpy(ssid, saved_ssid);
        strcpy(password, saved_password);
        return true;
    }
    return false;
}