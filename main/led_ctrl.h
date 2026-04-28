#ifndef LED_CTRL_H
#define LED_CTRL_H

#include <stdbool.h>

#define LED_GPIO_PIN 17

void led_init(void);
void led_start_task(void);
void led_set_override(bool active);

#endif // LED_CTRL_H
