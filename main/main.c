#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "led_ctrl.h"
#include "passthrough.h"
#include "web_server.h"

static const char *TAG = "MAIN";

#define BOOT_GPIO           0
#define FACTORY_RESET_MS    5000

/* Check if BOOT button (GPIO0, active-low) is held at startup.
 * If held for FACTORY_RESET_MS milliseconds, erase NVS and reboot
 * so the device comes up with factory defaults (AP enabled). */
static void check_factory_reset_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << BOOT_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    /* Not pressed – nothing to do */
    if (gpio_get_level(BOOT_GPIO) == 1) return;

    ESP_LOGI(TAG, "BOOT held – factory reset countdown (%d s)", FACTORY_RESET_MS / 1000);

    int elapsed = 0;
    while (elapsed < FACTORY_RESET_MS) {
        /* Fast blink: 100 ms on/off */
        gpio_set_level(LED_GPIO_PIN, (elapsed / 100) % 2);
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;

        if (gpio_get_level(BOOT_GPIO) == 1) {
            /* Released before timeout – cancel */
            gpio_set_level(LED_GPIO_PIN, 0);
            ESP_LOGI(TAG, "BOOT released – factory reset cancelled");
            return;
        }
    }

    /* Confirmed long press – perform factory reset */
    ESP_LOGI(TAG, "Factory reset triggered – erasing NVS");

    /* Rapid triple-blink to confirm */
    for (int i = 0; i < 6; i++) {
        gpio_set_level(LED_GPIO_PIN, i % 2);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    gpio_set_level(LED_GPIO_PIN, 0);

    nvs_flash_erase();
    esp_restart();
}

static int null_vprintf(const char *fmt, va_list args) {
    (void)fmt;
    (void)args;
    return 0;
}

void app_main(void) {
    // Suppress all log output on USB-CDC; uncomment to re-enable for debugging
    esp_log_set_vprintf(null_vprintf);

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

    // Init LED early so we can use it for factory-reset feedback
    led_init();

    // Long-press BOOT (GPIO0) for 5 s => factory reset
    check_factory_reset_button();

    // Load configuration
    load_config();

    // Start LED blink task
    led_start_task();
    
    // Initialize Passthrough (CDC settings)
    passthrough_init();
    
    // Initialize WiFi
    ESP_ERROR_CHECK(wifi_manager_init());
    
    // Wait for WiFi to initialize
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Start HTTP server
    ESP_ERROR_CHECK(web_server_start());
    
    // Auto-start passthrough
    ESP_LOGI(TAG, "Auto-starting passthrough...");
    passthrough_start();
    
    ESP_LOGI(TAG, "System initialized");
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&ip_info.ip));
    }
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
