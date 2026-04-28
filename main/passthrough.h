#ifndef PASSTHROUGH_H
#define PASSTHROUGH_H

#include <stdbool.h>
#include "esp_err.h"

void passthrough_init(void);
esp_err_t passthrough_start(void);
void passthrough_stop(void);
bool passthrough_is_running(void);
const char* passthrough_get_status(void);
void passthrough_send_to_network(const uint8_t *data, size_t len);

#endif // PASSTHROUGH_H
