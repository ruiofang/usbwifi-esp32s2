#include "app_config.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "CONFIG";

app_config_t config = {
    .sta_ssid = "",
    .sta_password = "",
    .passthrough_mode = "TCP_S",
    .remote_ip = "192.168.1.100",
    .remote_port = 8080,
    .local_port = 8888,
    .uart_baud = 115200,
    .enable_ap = true,
    .enable_sta = false
};

uint16_t sanitize_port_value(uint16_t port, uint16_t fallback_value) {
    return port == 0 ? fallback_value : port;
}

esp_err_t load_config(void) {
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
    nvs_get_str(handle, "sta_ssid", config.sta_ssid, &len);
    
    len = sizeof(config.sta_password);
    nvs_get_str(handle, "sta_password", config.sta_password, &len);
    
    len = sizeof(config.passthrough_mode);
    nvs_get_str(handle, "mode", config.passthrough_mode, &len);
    
    len = sizeof(config.remote_ip);
    nvs_get_str(handle, "remote_ip", config.remote_ip, &len);
    
    uint16_t val;
    if (nvs_get_u16(handle, "remote_port", &val) == ESP_OK) {
        config.remote_port = sanitize_port_value(val, 8080);
    }
    
    if (nvs_get_u16(handle, "local_port", &val) == ESP_OK) {
        config.local_port = sanitize_port_value(val, 8888);
    }

    uint32_t b32;
    if (nvs_get_u32(handle, "uart_baud", &b32) == ESP_OK) {
        config.uart_baud = b32;
    }
    
    uint8_t bval;
    if (nvs_get_u8(handle, "enable_ap", &bval) == ESP_OK) {
        config.enable_ap = bval;
    }
    
    if (nvs_get_u8(handle, "enable_sta", &bval) == ESP_OK) {
        config.enable_sta = bval;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Configuration loaded");
    return ESP_OK;
}

esp_err_t save_config(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "sta_ssid", config.sta_ssid);
    err |= nvs_set_str(handle, "sta_password", config.sta_password);
    err |= nvs_set_str(handle, "mode", config.passthrough_mode);
    err |= nvs_set_str(handle, "remote_ip", config.remote_ip);
    err |= nvs_set_u16(handle, "remote_port", config.remote_port);
    err |= nvs_set_u16(handle, "local_port", config.local_port);
    err |= nvs_set_u32(handle, "uart_baud", config.uart_baud);
    err |= nvs_set_u8(handle, "enable_ap", config.enable_ap);
    err |= nvs_set_u8(handle, "enable_sta", config.enable_sta);

    err |= nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuration saved");
    }
    return err;
}
