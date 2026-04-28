#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

esp_err_t web_server_start(void);
void web_server_ws_broadcast(const uint8_t *data, size_t len, uint8_t type);

#define WS_TYPE_SERIAL 0
#define WS_TYPE_NETWORK 1
#define WS_TYPE_SYSTEM  2

#endif // WEB_SERVER_H
