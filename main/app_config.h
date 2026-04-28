#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define AP_SSID "ESP32S2-USB-WIFI"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL 1
#define NVS_NAMESPACE "config"

typedef struct {
    char sta_ssid[32];
    char sta_password[64];
    char passthrough_mode[8];    // "TCP_S", "TCP_C", "UDP"
    char remote_ip[16];
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t uart_baud;
    bool enable_ap;
    bool enable_sta;
} app_config_t;

extern app_config_t config;

esp_err_t load_config(void);
esp_err_t save_config(void);
uint16_t sanitize_port_value(uint16_t port, uint16_t fallback_value);

#endif // APP_CONFIG_H
