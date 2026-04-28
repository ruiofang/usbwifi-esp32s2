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

/* Persistent task: monitors BOOT (GPIO0) at runtime.
 * Hold for 5 s → wait for release → erase NVS → restart.
 * Waiting for release prevents entering ROM download mode
 * (which happens when GPIO0 is LOW when the CPU comes out of reset). */
static void factory_reset_task(void *pvParameters)
{
    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << BOOT_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    uint32_t hold_cnt = 0;  /* each increment = 50 ms */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));

        if (gpio_get_level(BOOT_GPIO) == 0) {
            hold_cnt++;

            /* After 200 ms of continuous hold, take LED and show countdown blink.
             * Pattern: 200 ms ON / 200 ms OFF (4 ticks per state). */
            if (hold_cnt >= 4) {
                led_set_override(true);
                gpio_set_level(LED_GPIO_PIN, (hold_cnt / 4) % 2);
            }

            /* 5 s reached → perform factory reset */
            if (hold_cnt * 50 >= FACTORY_RESET_MS) {
                /* Solid LED while waiting for release */
                gpio_set_level(LED_GPIO_PIN, 1);
                while (gpio_get_level(BOOT_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }

                /* Triple-blink confirmation */
                for (int i = 0; i < 6; i++) {
                    gpio_set_level(LED_GPIO_PIN, i % 2);
                    vTaskDelay(pdMS_TO_TICKS(80));
                }
                gpio_set_level(LED_GPIO_PIN, 0);

                nvs_flash_erase();
                esp_restart();
            }
        } else {
            if (hold_cnt >= 4) {
                led_set_override(false);
            }
            hold_cnt = 0;
        }
    }
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

    // Load configuration
    load_config();

    // Start LED blink task + runtime BOOT-key factory-reset monitor
    led_start_task();
    xTaskCreate(factory_reset_task, "boot_btn", 2048, NULL, 5, NULL);
    
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
