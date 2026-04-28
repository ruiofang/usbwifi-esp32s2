#include "web_server.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "passthrough.h"

static httpd_handle_t server = NULL;

void web_server_ws_broadcast(const uint8_t *data, size_t len, uint8_t type) {
    if (server == NULL) return;
    
    uint8_t *msg = malloc(len + 1);
    if (!msg) return;
    msg[0] = type;
    memcpy(msg + 1, data, len);

    size_t fds = 10;
    int client_fds[10];
    if (httpd_get_client_list(server, &fds, client_fds) == ESP_OK) {
        for (size_t i = 0; i < fds; i++) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t ws_pkt = {
                    .payload = msg,
                    .len = len + 1,
                    .type = HTTPD_WS_TYPE_BINARY
                };
                httpd_ws_send_frame_async(server, client_fds[i], &ws_pkt);
            }
        }
    }
    free(msg);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) return ESP_ERR_NO_MEM;
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }

        if (ws_pkt.len >= 2) {
            uint8_t type = buf[0];
            uint8_t *data = buf + 1;
            size_t data_len = ws_pkt.len - 1;

            if (type == WS_TYPE_SERIAL) {
                // Send to Serial (USB CDC)
                // write() buffers in ROM s_usb_tx_buf; fsync() flushes it to USB FIFO.
                write(STDOUT_FILENO, data, data_len);
                fsync(STDOUT_FILENO);
            } else if (type == WS_TYPE_NETWORK) {
                // Send to Network
                passthrough_send_to_network(data, data_len);
            }
        }
    }
    free(buf);
    return ESP_OK;
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");

static bool extract_form_value(const char *body, const char *key, char *value_out, size_t value_out_size) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *value = strstr(body, pattern);
    if (value == NULL) return false;
    value += strlen(pattern);
    const char *end = strchr(value, '&');
    size_t value_len = (end != NULL) ? (size_t)(end - value) : strlen(value);
    if (value_out_size == 0) return false;
    if (value_len >= value_out_size) value_len = value_out_size - 1;
    memcpy(value_out, value, value_len);
    value_out[value_len] = '\0';
    return true;
}

static int parse_port_value(const char *value, uint16_t current_value) {
    if (value == NULL || *value == '\0') return current_value;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 65535) return current_value;
    return (uint16_t)parsed;
}

static esp_err_t http_index_handler(httpd_req_t *req) {
    static char response[32768];
    char status_class[20], status_text[64];
    static char wifi_list[8192];
    
    if (passthrough_is_running()) {
        strcpy(status_class, "status-running");
        strcpy(status_text, "Running");
    } else {
        strcpy(status_class, "status-stopped");
        strcpy(status_text, "Stopped");
    }

    if (scan_count > 0) {
        int pos = 0;
        pos += snprintf(wifi_list + pos, sizeof(wifi_list) - pos,
                       "<div class='section'>\n<h2>Available WiFi</h2>\n<div class='wifi-list'>\n");
        for (int i = 0; i < scan_count && pos < sizeof(wifi_list) - 200; i++) {
            char ssid_esc[33];
            strncpy(ssid_esc, (char*)scan_results[i].ssid, 32);
            ssid_esc[32] = '\0';
            pos += snprintf(wifi_list + pos, sizeof(wifi_list) - pos,
                           "<div class='wifi-item' onclick=\"document.getElementById('ssid-input').value='%s';\">\n"
                           "  <div class='wifi-ssid'>%s</div>\n"
                           "  <div class='wifi-info'>Ch: %d | RSSI: %d</div>\n"
                           "</div>\n",
                           ssid_esc, ssid_esc, scan_results[i].primary, scan_results[i].rssi);
        }
        pos += snprintf(wifi_list + pos, sizeof(wifi_list) - pos, "</div>\n</div>\n");
    } else {
        wifi_list[0] = '\0';
    }
    
    int len = snprintf(response, sizeof(response), (const char *)index_html_start,
                       status_class, status_text,
                       sta_status_text, sta_ip_text, sta_gw_text, sta_mask_text,
                       config.sta_ssid, config.sta_password,
                       config.enable_ap ? "checked" : "",
                       config.enable_sta ? "checked" : "",
                       wifi_list,
                       status_class, status_text,
                       passthrough_get_status(),
                       config.uart_baud,
                       strcmp(config.passthrough_mode, "TCP_S") == 0 ? "checked" : "",
                       strcmp(config.passthrough_mode, "TCP_C") == 0 ? "checked" : "",
                       strcmp(config.passthrough_mode, "UDP") == 0 ? "checked" : "",
                       config.remote_ip, config.remote_port, config.local_port,
                       config.uart_baud);

    if (len < 0 || len >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to render page");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

static esp_err_t http_redirect_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_scan_handler(httpd_req_t *req) {
    wifi_manager_scan();
    return http_redirect_handler(req);
}

static esp_err_t http_wifi_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret > 0) {
        buf[ret] = '\0';
        char ssid[32], password[64];
        if (extract_form_value(buf, "ssid", ssid, sizeof(ssid))) strncpy(config.sta_ssid, ssid, sizeof(config.sta_ssid));
        if (extract_form_value(buf, "password", password, sizeof(password))) strncpy(config.sta_password, password, sizeof(config.sta_password));
        config.enable_ap = (strstr(buf, "enable_ap") != NULL);
        config.enable_sta = (strstr(buf, "enable_sta") != NULL);
        save_config();
        esp_wifi_stop();
        wifi_manager_init();
    }
    return http_redirect_handler(req);
}

static esp_err_t http_passthrough_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret > 0) {
        buf[ret] = '\0';
        char mode[8], remote_ip[16], remote_port[8], local_port[8];
        if (extract_form_value(buf, "mode", mode, sizeof(mode))) strncpy(config.passthrough_mode, mode, sizeof(config.passthrough_mode));
        if (extract_form_value(buf, "remote_ip", remote_ip, sizeof(remote_ip))) strncpy(config.remote_ip, remote_ip, sizeof(config.remote_ip));
        if (extract_form_value(buf, "remote_port", remote_port, sizeof(remote_port))) config.remote_port = parse_port_value(remote_port, config.remote_port);
        if (extract_form_value(buf, "local_port", local_port, sizeof(local_port))) config.local_port = parse_port_value(local_port, config.local_port);
        save_config();
    }
    return http_redirect_handler(req);
}

static esp_err_t http_passthrough_start_handler(httpd_req_t *req) {
    passthrough_start();
    return http_redirect_handler(req);
}

static esp_err_t http_passthrough_stop_handler(httpd_req_t *req) {
    passthrough_stop();
    return http_redirect_handler(req);
}

static esp_err_t http_save_handler(httpd_req_t *req) {
    save_config();
    return http_redirect_handler(req);
}

static esp_err_t http_uart_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret > 0) {
        buf[ret] = '\0';
        char baud[16];
        if (extract_form_value(buf, "baud", baud, sizeof(baud))) {
            config.uart_baud = atoi(baud);
            save_config();
            passthrough_init();
        }
    }
    return http_redirect_handler(req);
}

esp_err_t web_server_start(void) {
    if (server != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 12;
    config.stack_size = 10240;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uris[] = {
            {.uri = "/", .method = HTTP_GET, .handler = http_index_handler},
            {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
            {.uri = "/wifi", .method = HTTP_POST, .handler = http_wifi_handler},
            {.uri = "/scan", .method = HTTP_POST, .handler = http_scan_handler},
            {.uri = "/passthrough", .method = HTTP_POST, .handler = http_passthrough_handler},
            {.uri = "/passthrough/start", .method = HTTP_POST, .handler = http_passthrough_start_handler},
            {.uri = "/passthrough/stop", .method = HTTP_POST, .handler = http_passthrough_stop_handler},
            {.uri = "/uart", .method = HTTP_POST, .handler = http_uart_handler},
            {.uri = "/save", .method = HTTP_POST, .handler = http_save_handler},
            {.uri = "/*", .method = HTTP_GET, .handler = http_redirect_handler}
        };
        for (int i = 0; i < sizeof(uris)/sizeof(httpd_uri_t); i++) {
            httpd_register_uri_handler(server, &uris[i]);
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}
