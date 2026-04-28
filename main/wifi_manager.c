#include "wifi_manager.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "lwip/inet.h"
#include "app_config.h"

static const char *TAG = "WIFI_MGR";

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static bool s_wifi_stack_initialized = false;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

wifi_ap_record_t scan_results[MAX_SCAN_RESULTS];
uint16_t scan_count = 0;
static bool scan_done = false;

char sta_status_text[24] = "Disabled";
char sta_ip_text[16] = "-";
char sta_gw_text[16] = "-";
char sta_mask_text[16] = "-";

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
            esp_wifi_connect();
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
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(sta_status_text, sizeof(sta_status_text), "Connected");
        snprintf(sta_ip_text, sizeof(sta_ip_text), IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(sta_gw_text, sizeof(sta_gw_text), IPSTR, IP2STR(&event->ip_info.gw));
        snprintf(sta_mask_text, sizeof(sta_mask_text), IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_scan(void) {
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
        return ret;
    }
    
    int timeout = 0;
    while (!scan_done && timeout < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout++;
    }
    
    if (!scan_done) {
        return ESP_ERR_TIMEOUT;
    }
    
    uint16_t max_ap = MAX_SCAN_RESULTS;
    ret = esp_wifi_scan_get_ap_records(&max_ap, scan_results);
    if (ret != ESP_OK) {
        return ret;
    }
    
    scan_count = max_ap;
    ESP_LOGI(TAG, "Found %d AP(s)", scan_count);
    return ESP_OK;
}

esp_err_t wifi_manager_init(void) {
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
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ap_ip_info);
    esp_netif_dhcps_start(s_ap_netif);
    
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
    } else if (!config.enable_ap && config.enable_sta && sta_available) {
        mode = WIFI_MODE_APSTA;
        config.enable_ap = true;
    } else if (config.enable_ap && config.enable_sta && sta_available) {
        mode = WIFI_MODE_APSTA;
    } else {
        mode = WIFI_MODE_AP;
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
    }
    
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}
