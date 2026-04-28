#include "led_ctrl.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "passthrough.h"

void led_init(void) {
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
        if (passthrough_is_running()) {
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

void led_start_task(void) {
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
}
