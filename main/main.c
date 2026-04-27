#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "driver/gpio.h"

#define TAG "USB-WIFI"

#define AP_SSID "ESP32S2-USB-WIFI"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL 1

#define LED_GPIO_PIN 17

#define NVS_NAMESPACE "config"

static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

static httpd_handle_t server = NULL;
static bool s_wifi_stack_initialized = false;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

typedef struct {
    char sta_ssid[32];
    char sta_password[64];
    char passthrough_mode[8];    // "TCP_S", "TCP_C", "UDP"
    char remote_ip[16];
    uint16_t remote_port;
    uint16_t local_port;
    bool enable_ap;
    bool enable_sta;
} app_config_t;

static app_config_t config = {
    .sta_ssid = "",
    .sta_password = "",
    .passthrough_mode = "TCP_S",
    .remote_ip = "192.168.1.100",
    .remote_port = 8080,
    .local_port = 8888,
    .enable_ap = true,
    .enable_sta = false
};

static bool passthrough_running = false;
static int passthrough_socket = -1;
static TaskHandle_t passthrough_task_handle = NULL;
void passthrough_task(void *pvParameters);
static uint16_t sanitize_port_value(uint16_t port, uint16_t fallback_value);

static int null_vprintf(const char *fmt, va_list args) {
    (void)fmt;
    (void)args;
    return 0;
}

static void configure_stdio_for_cdc_data(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
}

static void set_socket_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void bridge_socket_to_cdc(int sockfd, bool is_udp, struct sockaddr_in *udp_peer, bool *udp_peer_valid) {
    uint8_t network_buffer[1024];
    uint8_t cdc_buffer[256];

    set_socket_nonblocking(sockfd);

    while (passthrough_running) {
        bool idle = true;

        int cdc_len = read(STDIN_FILENO, cdc_buffer, sizeof(cdc_buffer));
        if (cdc_len > 0) {
            idle = false;

            if (is_udp) {
                if (*udp_peer_valid) {
                    ssize_t sent = sendto(sockfd, cdc_buffer, cdc_len, 0,
                                          (struct sockaddr *)udp_peer, sizeof(*udp_peer));
                    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        break;
                    }
                }
            } else {
                ssize_t sent = send(sockfd, cdc_buffer, cdc_len, 0);
                if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }
            }
        }

        if (is_udp) {
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            int net_len = recvfrom(sockfd, network_buffer, sizeof(network_buffer), 0,
                                   (struct sockaddr *)&from_addr, &from_len);
            if (net_len > 0) {
                idle = false;
                *udp_peer = from_addr;
                *udp_peer_valid = true;
                ssize_t written = write(STDOUT_FILENO, network_buffer, net_len);
                if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }
            } else if (net_len == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        } else {
            int net_len = recv(sockfd, network_buffer, sizeof(network_buffer), 0);
            if (net_len > 0) {
                idle = false;
                ssize_t written = write(STDOUT_FILENO, network_buffer, net_len);
                if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }
            } else if (net_len == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }

        if (idle) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

#define MAX_SCAN_RESULTS 10
static wifi_ap_record_t scan_results[MAX_SCAN_RESULTS];
static uint16_t scan_count = 0;
static bool scan_done = false;
static char sta_status_text[24] = "Disabled";
static char sta_ip_text[16] = "-";
static char sta_gw_text[16] = "-";
static char sta_mask_text[16] = "-";

static TaskHandle_t led_task_handle = NULL;

static void led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO_PIN, 0);
}

static void led_task(void *pvParameters) {
    while (1) {
        if (passthrough_running) {
            gpio_set_level(LED_GPIO_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_GPIO_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            gpio_set_level(LED_GPIO_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(LED_GPIO_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static esp_err_t save_config(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "sta_ssid", config.sta_ssid);
    err |= nvs_set_str(handle, "sta_password", config.sta_password);
    err |= nvs_set_str(handle, "mode", config.passthrough_mode);
    err |= nvs_set_str(handle, "remote_ip", config.remote_ip);
    err |= nvs_set_u16(handle, "remote_port", config.remote_port);
    err |= nvs_set_u16(handle, "local_port", config.local_port);
    err |= nvs_set_u8(handle, "enable_ap", config.enable_ap);
    err |= nvs_set_u8(handle, "enable_sta", config.enable_sta);

    err |= nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuration saved");
    }
    return err;
}

static esp_err_t load_config(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No saved config found, using defaults");
            return ESP_OK;
        }
        return err;
    }

    size_t len;
    
    len = sizeof(config.sta_ssid);
    err = nvs_get_str(handle, "sta_ssid", config.sta_ssid, &len);
    
    len = sizeof(config.sta_password);
    err |= nvs_get_str(handle, "sta_password", config.sta_password, &len);
    
    len = sizeof(config.passthrough_mode);
    err |= nvs_get_str(handle, "mode", config.passthrough_mode, &len);
    
    len = sizeof(config.remote_ip);
    err |= nvs_get_str(handle, "remote_ip", config.remote_ip, &len);
    
    uint16_t val;
    esp_err_t port_err = nvs_get_u16(handle, "remote_port", &val);
    err |= port_err;
    if (port_err == ESP_OK) config.remote_port = sanitize_port_value(val, 8080);
    
    port_err = nvs_get_u16(handle, "local_port", &val);
    err |= port_err;
    if (port_err == ESP_OK) config.local_port = sanitize_port_value(val, 8888);
    
    uint8_t bval;
    err |= nvs_get_u8(handle, "enable_ap", &bval);
    if (err == ESP_OK) config.enable_ap = bval;
    
    err |= nvs_get_u8(handle, "enable_sta", &bval);
    if (err == ESP_OK) config.enable_sta = bval;

    nvs_close(handle);

    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Configuration loaded");
        return ESP_OK;
    }
    return err;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "WiFi AP: station connected");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "WiFi AP: station disconnected");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        snprintf(sta_status_text, sizeof(sta_status_text), "Connecting");
        ESP_LOGI(TAG, "WiFi STA started");
        if (config.enable_sta && strlen(config.sta_ssid) > 0) {
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start STA connection: %s", esp_err_to_name(ret));
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        scan_done = true;
        ESP_LOGI(TAG, "WiFi scan done");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        snprintf(sta_status_text, sizeof(sta_status_text), "Disconnected");
        snprintf(sta_ip_text, sizeof(sta_ip_text), "-");
        snprintf(sta_gw_text, sizeof(sta_gw_text), "-");
        snprintf(sta_mask_text, sizeof(sta_mask_text), "-");
        ESP_LOGI(TAG, "WiFi STA disconnected");
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (config.enable_sta && strlen(config.sta_ssid) > 0) {
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to reconnect STA: %s", esp_err_to_name(ret));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(sta_status_text, sizeof(sta_status_text), "Connected");
        snprintf(sta_ip_text, sizeof(sta_ip_text), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(sta_gw_text, sizeof(sta_gw_text), IPSTR, IP2STR(&event->ip_info.gw));
        snprintf(sta_mask_text, sizeof(sta_mask_text), IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA Gateway: " IPSTR ", Netmask: " IPSTR,
                 IP2STR(&event->ip_info.gw), IP2STR(&event->ip_info.netmask));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t scan_wifi(void) {
    scan_done = false;
    scan_count = 0;
    
    ESP_LOGI(TAG, "Starting WiFi scan...");
    
    wifi_scan_config_t scan_config = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 200,
    };
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scan: %s", esp_err_to_name(ret));
        return ret;
    }
    
    int timeout = 0;
    while (!scan_done && timeout < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout++;
    }
    
    if (!scan_done) {
        ESP_LOGE(TAG, "Scan timeout");
        return ESP_ERR_TIMEOUT;
    }
    
    uint16_t max_ap = MAX_SCAN_RESULTS;
    ret = esp_wifi_scan_get_ap_records(&max_ap, scan_results);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan results: %s", esp_err_to_name(ret));
        return ret;
    }
    
    scan_count = max_ap;
    ESP_LOGI(TAG, "Found %d AP(s)", scan_count);
    
    for (int i = 0; i < scan_count; i++) {
        ESP_LOGI(TAG, "  %d: %s (ch:%d, rssi:%d)", i, 
                 scan_results[i].ssid, 
                 scan_results[i].primary, 
                 scan_results[i].rssi);
    }
    
    return ESP_OK;
}

static esp_err_t wifi_init(void) {
    if (!s_wifi_stack_initialized) {
        s_wifi_event_group = xEventGroupCreate();

        ESP_ERROR_CHECK(esp_netif_init());

        esp_err_t ret = esp_event_loop_create_default();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return ret;
        }

        s_ap_netif = esp_netif_create_default_wifi_ap();
        s_sta_netif = esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

        s_wifi_stack_initialized = true;
    }
    
    esp_netif_ip_info_t ap_ip_info;
    memset(&ap_ip_info, 0, sizeof(ap_ip_info));
    ap_ip_info.ip.addr = ipaddr_addr("192.168.4.1");
    ap_ip_info.gw.addr = ipaddr_addr("192.168.4.1");
    ap_ip_info.netmask.addr = ipaddr_addr("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ap_ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    
    wifi_config_t ap_config = {0};
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    memcpy(ap_config.ap.ssid, AP_SSID, strlen(AP_SSID));
    ap_config.ap.ssid_len = strlen(AP_SSID);
    ap_config.ap.channel = AP_CHANNEL;
    memcpy(ap_config.ap.password, AP_PASSWORD, strlen(AP_PASSWORD));
    
    wifi_mode_t mode = WIFI_MODE_APSTA;
    bool sta_available = (strlen(config.sta_ssid) > 0);
    
    if (config.enable_ap && !config.enable_sta) {
        mode = WIFI_MODE_AP;
        ESP_LOGI(TAG, "WiFi mode: AP only");
    } else if (!config.enable_ap && config.enable_sta && sta_available) {
        mode = WIFI_MODE_APSTA;
        config.enable_ap = true;
        ESP_LOGI(TAG, "WiFi mode: AP+STA (management AP forced on)");
    } else if (config.enable_ap && config.enable_sta && sta_available) {
        mode = WIFI_MODE_APSTA;
        ESP_LOGI(TAG, "WiFi mode: AP+STA");
    } else {
        mode = WIFI_MODE_AP;
        ESP_LOGI(TAG, "WiFi mode: AP only (STA SSID empty)");
        config.enable_ap = true;
        config.enable_sta = false;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    
    if (config.enable_sta && sta_available) {
        wifi_config_t sta_config = {0};
        sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        sta_config.sta.threshold.rssi = -127;
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        memcpy(sta_config.sta.ssid, config.sta_ssid, strlen(config.sta_ssid));
        memcpy(sta_config.sta.password, config.sta_password, strlen(config.sta_password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_LOGI(TAG, "STA config set: %s", config.sta_ssid);
    }
    
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi initialized. AP SSID: %s, Channel: %d", AP_SSID, AP_CHANNEL);
    ESP_LOGI(TAG, "AP IP Address: 192.168.4.1");
    return ESP_OK;
}

static const char* http_index_html = 
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>\n"
    "<title>ESP32-S2 USB WiFi Config</title>\n"
    "<style>\n"
    "* { box-sizing: border-box; }\n"
    "html, body { margin: 0; padding: 0; height: 100%; }\n"
    "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 16px; }\n"
    ".container { background: white; border-radius: 16px; padding: 20px; box-shadow: 0 10px 40px rgba(0,0,0,0.15); max-width: 500px; margin: 0 auto; }\n"
    "h1 { color: #333; font-size: 20px; margin: 0 0 16px; padding-bottom: 12px; border-bottom: 1px solid #eee; }\n"
    ".section { margin-bottom: 16px; }\n"
    ".section h2 { color: #555; font-size: 13px; font-weight: 600; margin: 0 0 10px; text-transform: uppercase; letter-spacing: 0.5px; }\n"
    ".form-group { margin-bottom: 10px; }\n"
    ".form-group label { display: block; color: #666; font-size: 12px; margin-bottom: 4px; }\n"
    ".form-group input, .form-group select { width: 100%; padding: 10px 12px; border: 1px solid #ddd; border-radius: 8px; font-size: 14px; transition: border-color 0.2s; }\n"
    ".form-group input:focus, .form-group select:focus { outline: none; border-color: #667eea; }\n"
    ".form-group input[type='checkbox'] { width: auto; margin-right: 6px; }\n"
    ".btn { width: 100%; padding: 12px; border: none; border-radius: 8px; font-size: 14px; font-weight: 600; cursor: pointer; transition: all 0.2s; margin-bottom: 8px; -webkit-tap-highlight-color: transparent; }\n"
    ".btn-primary { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; }\n"
    ".btn-primary:active { transform: translateY(1px); }\n"
    ".btn-secondary { background: #f5f5f5; color: #666; }\n"
    ".btn-secondary:active { background: #e5e5e5; }\n"
    ".status { padding: 12px; border-radius: 8px; margin-bottom: 14px; text-align: center; font-size: 13px; }\n"
    ".status-running { background: #d4edda; color: #155724; }\n"
    ".status-stopped { background: #f8d7da; color: #721c24; }\n"
    ".row { display: flex; gap: 10px; }\n"
    ".row .form-group { flex: 1; }\n"
    ".radio-group { display: flex; flex-direction: column; gap: 8px; }\n"
    ".radio-group label { font-weight: normal; cursor: pointer; display: flex; align-items: center; }\n"
    ".wifi-list { max-height: 200px; overflow-y: auto; border: 1px solid #ddd; border-radius: 8px; }\n"
    ".wifi-item { padding: 10px; border-bottom: 1px solid #eee; cursor: pointer; transition: background 0.2s; }\n"
    ".wifi-item:active { background: #f0f0f0; }\n"
    ".wifi-item:last-child { border-bottom: none; }\n"
    ".wifi-ssid { font-weight: 600; color: #333; word-break: break-all; }\n"
    ".wifi-info { font-size: 11px; color: #999; margin-top: 2px; }\n"
    ".info-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }\n"
    ".info-card { background: #f7f8fb; border: 1px solid #e7e9f0; border-radius: 8px; padding: 10px 12px; }\n"
    ".info-card-label { color: #7b8190; font-size: 11px; text-transform: uppercase; letter-spacing: 0.4px; margin-bottom: 4px; }\n"
    ".info-card-value { color: #2f3440; font-size: 13px; font-weight: 600; word-break: break-all; }\n"
    "footer { text-align: center; color: rgba(255,255,255,0.8); font-size: 11px; margin-top: 12px; }\n"
    "@media (max-width: 400px) {\n"
    "  .container { padding: 16px; }\n"
    "  h1 { font-size: 18px; margin-bottom: 12px; }\n"
    "  .section { margin-bottom: 12px; }\n"
    "  .row { flex-direction: column; gap: 0; }\n"
    "  .info-grid { grid-template-columns: 1fr; }\n"
    "  body { padding: 12px; }\n"
    "}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<div class='container'>\n"
    "<h1>ESP32-S2 USB WiFi</h1>\n"
    "<div class='status %s'>Status: %s</div>\n"
    "<div class='section'>\n"
    "<h2>WiFi Settings</h2>\n"
    "<form action='/scan' method='POST'>\n"
    "<button type='submit' class='btn btn-primary'>Scan WiFi</button>\n"
    "</form>\n"
    "<form action='/wifi' method='POST'>\n"
    "<div class='form-group'><label>WiFi SSID</label><input type='text' name='ssid' value='%s' placeholder='WiFi name' id='ssid-input'></div>\n"
    "<div class='form-group'><label>Password</label><input type='password' name='password' value='%s' placeholder='WiFi password'></div>\n"
    "<div class='form-group'>\n"
    "<label><input type='checkbox' name='enable_ap' %s> Enable AP Mode</label>\n"
    "<label><input type='checkbox' name='enable_sta' %s> Enable STA Mode</label>\n"
    "</div>\n"
    "<button type='submit' class='btn btn-primary'>Save WiFi</button>\n"
    "</form>\n"
    "<div class='info-grid'>\n"
    "<div class='info-card'><div class='info-card-label'>STA Status</div><div class='info-card-value'>%s</div></div>\n"
    "<div class='info-card'><div class='info-card-label'>STA IP</div><div class='info-card-value'>%s</div></div>\n"
    "<div class='info-card'><div class='info-card-label'>Gateway</div><div class='info-card-value'>%s</div></div>\n"
    "<div class='info-card'><div class='info-card-label'>Netmask</div><div class='info-card-value'>%s</div></div>\n"
    "</div>\n"
    "%s"
    "</div>\n"
    "<div class='section'>\n"
    "<h2>TCP/UDP Passthrough</h2>\n"
    "<form action='/passthrough' method='POST'>\n"
    "<div class='form-group'>\n"
    "<label>Mode:</label>\n"
    "<div class='radio-group'>\n"
    "<label><input type='radio' name='mode' value='TCP_S' %s> TCP Server</label>\n"
    "<label><input type='radio' name='mode' value='TCP_C' %s> TCP Client</label>\n"
    "<label><input type='radio' name='mode' value='UDP' %s> UDP</label>\n"
    "</div>\n"
    "</div>\n"
    "<div class='row'>\n"
    "<div class='form-group'><label>Remote IP</label><input type='text' name='remote_ip' value='%s' placeholder='192.168.1.100'></div>\n"
    "<div class='form-group'><label>Remote Port</label><input type='number' name='remote_port' value='%d' min='1' max='65535'></div>\n"
    "</div>\n"
    "<div class='form-group'><label>Local Port</label><input type='number' name='local_port' value='%d' min='1' max='65535'></div>\n"
    "<button type='submit' class='btn btn-primary'>Save Settings</button>\n"
    "</form>\n"
    "</div>\n"
    "<div class='section'>\n"
    "<h2>Control</h2>\n"
    "<form action='/passthrough/start' method='POST'><button type='submit' class='btn btn-primary'>Start</button></form>\n"
    "<form action='/passthrough/stop' method='POST'><button type='submit' class='btn btn-secondary'>Stop</button></form>\n"
    "<form action='/save' method='POST'><button type='submit' class='btn btn-secondary'>Save All Config</button></form>\n"
    "</div>\n"
    "</div>\n"
    "<footer>ESP32-S2 USB WiFi v1.0</footer>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t http_index_handler(httpd_req_t *req) {
    static char response[10240];
    char status_class[20], status_text[20];
    static char wifi_list[2048];
    
    ESP_LOGI(TAG, "HTTP request received for /");
    
    if (passthrough_running) {
        strcpy(status_class, "status-running");
        strcpy(status_text, "Passthrough Running");
    } else {
        strcpy(status_class, "status-stopped");
        strcpy(status_text, "Passthrough Stopped");
    }
    
    if (scan_count > 0) {
        int pos = 0;
        pos += snprintf(wifi_list + pos, sizeof(wifi_list) - pos,
                       "<div class='section'>\n"
                       "<h2>Available WiFi</h2>\n"
                       "<div class='wifi-list'>\n");
        
        for (int i = 0; i < scan_count && pos < sizeof(wifi_list) - 200; i++) {
            char ssid_esc[33];
            int j = 0;
            for (j = 0; j < strlen((char*)scan_results[i].ssid) && j < 32; j++) {
                ssid_esc[j] = scan_results[i].ssid[j];
            }
            ssid_esc[j] = '\0';
            
            pos += snprintf(wifi_list + pos, sizeof(wifi_list) - pos,
                           "<div class='wifi-item' onclick=\"document.getElementById('ssid-input').value='%s';\">\n"
                           "  <div class='wifi-ssid'>%s</div>\n"
                           "  <div class='wifi-info'>Ch: %d | RSSI: %d</div>\n"
                           "</div>\n",
                           ssid_esc, ssid_esc,
                           scan_results[i].primary, scan_results[i].rssi);
        }
        pos += snprintf(wifi_list + pos, sizeof(wifi_list) - pos,
                       "</div>\n</div>\n");
    } else {
        wifi_list[0] = '\0';
    }
    
    int len = snprintf(response, sizeof(response), http_index_html,
             status_class, status_text,
             config.sta_ssid, config.sta_password,
             config.enable_ap ? "checked" : "",
             config.enable_sta ? "checked" : "",
             sta_status_text, sta_ip_text, sta_gw_text, sta_mask_text,
             wifi_list,
             strcmp(config.passthrough_mode, "TCP_S") == 0 ? "checked" : "",
             strcmp(config.passthrough_mode, "TCP_C") == 0 ? "checked" : "",
             strcmp(config.passthrough_mode, "UDP") == 0 ? "checked" : "",
             config.remote_ip, config.remote_port, config.local_port);

    if (len < 0 || len >= sizeof(response)) {
        ESP_LOGE(TAG, "Failed to generate HTTP response, len=%d", len);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to render page");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "HTTP response generated, length=%d", len);
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

static esp_err_t http_catch_all_get_handler(httpd_req_t *req) {
    ESP_LOGW(TAG, "Unhandled GET URI: %s, redirecting to /", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_scan_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "WiFi scan requested");
    
    scan_wifi();
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static bool extract_form_value(const char *body, const char *key, char *value_out, size_t value_out_size) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", key);

    const char *value = strstr(body, pattern);
    if (value == NULL) {
        return false;
    }

    value += strlen(pattern);
    const char *end = strchr(value, '&');
    size_t value_len = (end != NULL) ? (size_t)(end - value) : strlen(value);

    if (value_out_size == 0) {
        return false;
    }

    if (value_len >= value_out_size) {
        value_len = value_out_size - 1;
    }

    memcpy(value_out, value, value_len);
    value_out[value_len] = '\0';
    return true;
}

static int parse_port_value(const char *value, uint16_t current_value) {
    if (value == NULL || *value == '\0') {
        return current_value;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
        return current_value;
    }

    return (uint16_t)parsed;
}

static uint16_t sanitize_port_value(uint16_t port, uint16_t fallback_value) {
    return port == 0 ? fallback_value : port;
}

static esp_err_t http_wifi_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret > 0) {
        buf[ret] = '\0';
        
        char* ssid = strstr(buf, "ssid=");
        char* password = strstr(buf, "password=");
        char* enable_ap = strstr(buf, "enable_ap");
        char* enable_sta = strstr(buf, "enable_sta");
        
        if (ssid) {
            ssid += 5;
            char* end = strchr(ssid, '&');
            if (end) *end = '\0';
            strncpy(config.sta_ssid, ssid, sizeof(config.sta_ssid)-1);
        }
        
        if (password) {
            password += 9;
            char* end = strchr(password, '&');
            if (end) *end = '\0';
            strncpy(config.sta_password, password, sizeof(config.sta_password)-1);
        }
        
        config.enable_ap = (enable_ap != NULL);
        config.enable_sta = (enable_sta != NULL);
        
        save_config();
        
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(wifi_init());
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_passthrough_handler(httpd_req_t *req) {
    char buf[256];
    int total_len = req->content_len;
    if (total_len > 0 && total_len < sizeof(buf)) {
        int received = 0;
        while (received < total_len) {
            int ret = httpd_req_recv(req, buf + received, total_len - received);
            if (ret <= 0) {
                break;
            }
            received += ret;
        }

        if (received == total_len) {
            buf[received] = '\0';

            char mode[sizeof(config.passthrough_mode)] = {0};
            char remote_ip[sizeof(config.remote_ip)] = {0};
            char remote_port[8] = {0};
            char local_port[8] = {0};

            if (extract_form_value(buf, "mode", mode, sizeof(mode))) {
                strncpy(config.passthrough_mode, mode, sizeof(config.passthrough_mode) - 1);
                config.passthrough_mode[sizeof(config.passthrough_mode) - 1] = '\0';
            }

            if (extract_form_value(buf, "remote_ip", remote_ip, sizeof(remote_ip))) {
                strncpy(config.remote_ip, remote_ip, sizeof(config.remote_ip) - 1);
                config.remote_ip[sizeof(config.remote_ip) - 1] = '\0';
            }

            if (extract_form_value(buf, "remote_port", remote_port, sizeof(remote_port))) {
                config.remote_port = parse_port_value(remote_port, config.remote_port);
            }

            if (extract_form_value(buf, "local_port", local_port, sizeof(local_port))) {
                config.local_port = parse_port_value(local_port, config.local_port);
            }

            config.remote_port = sanitize_port_value(config.remote_port, 8080);
            config.local_port = sanitize_port_value(config.local_port, 8888);

            save_config();
        }
    }
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void stop_passthrough(void) {
    if (passthrough_running && passthrough_task_handle) {
        vTaskDelete(passthrough_task_handle);
        passthrough_task_handle = NULL;
    }
    if (passthrough_socket >= 0) {
        close(passthrough_socket);
        passthrough_socket = -1;
    }
    passthrough_running = false;
}

static esp_err_t http_passthrough_start_handler(httpd_req_t *req) {
    stop_passthrough();
    passthrough_running = true;
    xTaskCreate(passthrough_task, "passthrough_task", 4096, NULL, 5, &passthrough_task_handle);
    ESP_LOGI(TAG, "Passthrough started: %s", config.passthrough_mode);
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_passthrough_stop_handler(httpd_req_t *req) {
    stop_passthrough();
    ESP_LOGI(TAG, "Passthrough stopped");
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_save_handler(httpd_req_t *req) {
    save_config();
    ESP_LOGI(TAG, "All configuration saved");
    
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = http_index_handler};
static httpd_uri_t wifi_uri = {.uri = "/wifi", .method = HTTP_POST, .handler = http_wifi_handler};
static httpd_uri_t scan_uri = {.uri = "/scan", .method = HTTP_POST, .handler = http_scan_handler};
static httpd_uri_t passthrough_uri = {.uri = "/passthrough", .method = HTTP_POST, .handler = http_passthrough_handler};
static httpd_uri_t passthrough_start_uri = {.uri = "/passthrough/start", .method = HTTP_POST, .handler = http_passthrough_start_handler};
static httpd_uri_t passthrough_stop_uri = {.uri = "/passthrough/stop", .method = HTTP_POST, .handler = http_passthrough_stop_handler};
static httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = http_save_handler};
static httpd_uri_t catch_all_get_uri = {.uri = "/*", .method = HTTP_GET, .handler = http_catch_all_get_handler};

static esp_err_t http_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.max_resp_headers = 16;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &wifi_uri);
        httpd_register_uri_handler(server, &scan_uri);
        httpd_register_uri_handler(server, &passthrough_uri);
        httpd_register_uri_handler(server, &passthrough_start_uri);
        httpd_register_uri_handler(server, &passthrough_stop_uri);
        httpd_register_uri_handler(server, &save_uri);
        httpd_register_uri_handler(server, &catch_all_get_uri);
        ESP_LOGI(TAG, "HTTP server started successfully");
        ESP_LOGI(TAG, "HTTP server available at http://192.168.4.1");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}

void passthrough_task(void *pvParameters) {
    int sockfd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    
    while (passthrough_running) {
        if (strcmp(config.passthrough_mode, "TCP_S") == 0) {
            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                ESP_LOGE(TAG, "Failed to create TCP server socket");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            int opt = 1;
            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            
            memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(config.local_port);
            server_addr.sin_addr.s_addr = INADDR_ANY;
            
            if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                ESP_LOGE(TAG, "TCP server bind failed");
                close(sockfd);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            listen(sockfd, 1);
            ESP_LOGI(TAG, "TCP Server listening on port %d", config.local_port);
            
            client_len = sizeof(client_addr);
            client_fd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd >= 0) {
                bridge_socket_to_cdc(client_fd, false, NULL, NULL);
                close(client_fd);
            }
            close(sockfd);
            
        } else if (strcmp(config.passthrough_mode, "TCP_C") == 0) {
            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                ESP_LOGE(TAG, "Failed to create TCP client socket");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(config.remote_port);
            inet_pton(AF_INET, config.remote_ip, &server_addr.sin_addr);
            
            if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                close(sockfd);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            bridge_socket_to_cdc(sockfd, false, NULL, NULL);
            
            close(sockfd);
            
        } else {
            sockfd = socket(AF_INET, SOCK_DGRAM, 0);
            if (sockfd < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(config.local_port);
            server_addr.sin_addr.s_addr = INADDR_ANY;
            
            if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
                close(sockfd);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            bool udp_peer_valid = false;
            memset(&client_addr, 0, sizeof(client_addr));
            if (inet_pton(AF_INET, config.remote_ip, &client_addr.sin_addr) == 1 && config.remote_port > 0) {
                client_addr.sin_family = AF_INET;
                client_addr.sin_port = htons(config.remote_port);
                udp_peer_valid = true;
            }

            bridge_socket_to_cdc(sockfd, true, &client_addr, &udp_peer_valid);
            
            close(sockfd);
        }
        
        if (passthrough_running) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    
    vTaskDelete(NULL);
}

void app_main(void) {
    esp_log_set_vprintf(null_vprintf);
    configure_stdio_for_cdc_data();

    ESP_LOGI(TAG, "Starting ESP32-S2 USB WiFi Project");
    
    led_init();
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, &led_task_handle);
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS needs erase...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    } else if (ret != ESP_OK) {
        ESP_LOGI(TAG, "NVS init error, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    ESP_LOGI(TAG, "Loading config...");
    load_config();
    
    if (strlen(config.sta_ssid) == 0 && config.enable_sta) {
        ESP_LOGI(TAG, "STA SSID empty but STA enabled, resetting to default...");
        config.enable_ap = true;
        config.enable_sta = false;
    }
    
    ESP_LOGI(TAG, "Initializing WiFi...");
    ESP_ERROR_CHECK(wifi_init());
    
    ESP_LOGI(TAG, "Waiting for WiFi to initialize...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "Starting HTTP server...");
    ESP_ERROR_CHECK(http_server_start());
    
    ESP_LOGI(TAG, "System initialized");
    ESP_LOGI(TAG, "AP SSID: %s, Password: %s", AP_SSID, AP_PASSWORD);
    ESP_LOGI(TAG, "Access http://192.168.4.1 to configure");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
