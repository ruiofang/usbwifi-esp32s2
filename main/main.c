#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "led_ctrl.h"
#include "passthrough.h"
#include "web_server.h"

static const char *TAG = "MAIN";

static int null_vprintf(const char *fmt, va_list args) {
    (void)fmt;
    (void)args;
    return 0;
}

void app_main(void) {
    // Enable logging for troubleshooting USB issues
    // esp_log_set_vprintf(null_vprintf);

    ESP_LOGI(TAG, "Starting ESP32-S2 USB WiFi Project");
    ESP_LOGI(TAG, "Wait for USB stabilization...");
    
    // Give USB some time to stabilize
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Load configuration
    load_config();
    
    // Initialize LED
    led_init();
    led_start_task();
    
    // Initialize Passthrough (CDC settings)
    passthrough_init();
    
    // Initialize WiFi
    ESP_ERROR_CHECK(wifi_manager_init());
    
    // Wait for WiFi to initialize
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Start HTTP server
    ESP_ERROR_CHECK(web_server_start());
    
    ESP_LOGI(TAG, "System initialized");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
