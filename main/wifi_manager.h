#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi.h"

#define MAX_SCAN_RESULTS 10

extern wifi_ap_record_t scan_results[MAX_SCAN_RESULTS];
extern uint16_t scan_count;
extern char sta_status_text[24];
extern char sta_ip_text[16];
extern char sta_gw_text[16];
extern char sta_mask_text[16];

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_scan(void);

#endif // WIFI_MANAGER_H
