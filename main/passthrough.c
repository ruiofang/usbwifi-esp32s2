#include "passthrough.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "app_config.h"
#include "web_server.h"
#include "driver/uart.h"

static bool passthrough_running = false;
static int passthrough_socket = -1;
static TaskHandle_t passthrough_task_handle = NULL;
static char s_status[64] = "Stopped";

const char* passthrough_get_status(void) {
    return s_status;
}

void passthrough_send_to_network(const uint8_t *data, size_t len) {
    if (passthrough_running && passthrough_socket >= 0) {
        if (strcmp(config.passthrough_mode, "UDP") == 0) {
            struct sockaddr_in dest_addr;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(config.remote_port);
            inet_pton(AF_INET, config.remote_ip, &dest_addr.sin_addr);
            sendto(passthrough_socket, data, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        } else {
            send(passthrough_socket, data, len, 0);
        }
    }
}

static void bridge_socket_to_cdc(int sockfd, bool is_udp, struct sockaddr_in *udp_peer, bool *udp_peer_valid) {
    uint8_t buffer[2048];

    ESP_LOGI("PASSTHROUGH", "Bridge started: %s", is_udp ? "UDP" : "TCP");

    // Set socket non-blocking so both directions can be polled in one loop.
    // ESP-IDF's select() does not reliably mix VFS fds (STDIN) with lwip
    // socket fds, causing the network->serial direction to never trigger.
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    while (passthrough_running) {
        bool activity = false;

        // Direction 1: CDC (Serial) -> Network
        int len = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (len > 0) {
            activity = true;
            ESP_LOGI("PASSTHROUGH", "CDC -> Net: %d bytes", len);
            web_server_ws_broadcast(buffer, len, WS_TYPE_SERIAL);
            int sent;
            if (is_udp) {
                if (udp_peer_valid && *udp_peer_valid) {
                    sent = sendto(sockfd, buffer, len, 0, (struct sockaddr *)udp_peer, sizeof(*udp_peer));
                } else {
                    sent = 0;
                }
            } else {
                sent = send(sockfd, buffer, len, 0);
            }
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE("PASSTHROUGH", "Socket send error: %d", errno);
                break;
            }
        }

        // Direction 2: Network -> CDC (Serial)
        if (is_udp) {
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            len = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
            if (len > 0 && udp_peer && udp_peer_valid) {
                *udp_peer = from_addr;
                *udp_peer_valid = true;
            }
        } else {
            len = recv(sockfd, buffer, sizeof(buffer), 0);
        }

        if (len > 0) {
            activity = true;
            ESP_LOGI("PASSTHROUGH", "Net -> CDC: %d bytes", len);
            web_server_ws_broadcast(buffer, len, WS_TYPE_NETWORK);
            int written = 0, retries = 0;
            while (written < len && passthrough_running) {
                int ret = write(STDOUT_FILENO, buffer + written, len - written);
                if (ret > 0) {
                    written += ret;
                    retries = 0;
                } else if (ret == 0 || (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                    // cdcacm_write() only auto-flushes on '\n' or buffer-full;
                    // fsync() calls cdcacm_fsync() -> esp_usb_console_flush() to force send.
                    fsync(STDOUT_FILENO);
                    vTaskDelay(pdMS_TO_TICKS(5));
                    if (++retries > 200) {
                        ESP_LOGE("PASSTHROUGH", "CDC write timeout");
                        break;
                    }
                } else {
                    ESP_LOGE("PASSTHROUGH", "CDC write error: %d", errno);
                    goto bridge_exit;
                }
            }
            // Force flush: cdcacm_write buffers data until '\n' or buffer-full.
            // fsync() -> cdcacm_fsync() -> esp_usb_console_flush() -> cdc_acm_fifo_fill()
            // actually transmits the USB packet regardless of newlines.
            fsync(STDOUT_FILENO);
        } else if (len == 0) {
            ESP_LOGI("PASSTHROUGH", "Network connection closed");
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE("PASSTHROUGH", "Network recv error: %d", errno);
            break;
        }

        // Yield when both directions are idle to avoid busy-waiting
        if (!activity) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
bridge_exit:;
}

static void passthrough_task(void *pvParameters) {
    int sockfd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    
    while (passthrough_running) {
        if (strcmp(config.passthrough_mode, "TCP_S") == 0) {
            snprintf(s_status, sizeof(s_status), "TCP Server: Listening on %d", config.local_port);
            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd >= 0) {
                int opt = 1;
                setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                memset(&server_addr, 0, sizeof(server_addr));
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(config.local_port);
                server_addr.sin_addr.s_addr = INADDR_ANY;
                
                if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                    if (listen(sockfd, 1) != 0) {
                        ESP_LOGE("PASSTHROUGH", "Listen failed: %d", errno);
                        close(sockfd);
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        continue;
                    }
                    
                    client_len = sizeof(client_addr);
                    client_fd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd >= 0) {
                        // Set non-blocking for bridge
                        int flags = fcntl(client_fd, F_GETFL, 0);
                        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
                        
                        char client_ip[16];
                        inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
                        ESP_LOGI("PASSTHROUGH", "TCP Client connected: %s", client_ip);
                        snprintf(s_status, sizeof(s_status), "TCP Server: Connected to %s", client_ip);
                        passthrough_socket = client_fd;
                        bridge_socket_to_cdc(client_fd, false, NULL, NULL);
                        passthrough_socket = -1;
                        close(client_fd);
                        ESP_LOGI("PASSTHROUGH", "TCP Client disconnected");
                        snprintf(s_status, sizeof(s_status), "TCP Server: Listening on %d", config.local_port);
                    }
                } else {
                    ESP_LOGE("PASSTHROUGH", "Bind failed on port %d: %d", config.local_port, errno);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                close(sockfd);
            }
        } else if (strcmp(config.passthrough_mode, "TCP_C") == 0) {
            snprintf(s_status, sizeof(s_status), "TCP Client: Connecting to %s:%d", config.remote_ip, config.remote_port);
            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd >= 0) {
                memset(&server_addr, 0, sizeof(server_addr));
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(config.remote_port);
                inet_pton(AF_INET, config.remote_ip, &server_addr.sin_addr);
                if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                    snprintf(s_status, sizeof(s_status), "TCP Client: Connected to %s:%d", config.remote_ip, config.remote_port);
                    passthrough_socket = sockfd;
                    bridge_socket_to_cdc(sockfd, false, NULL, NULL);
                    passthrough_socket = -1;
                }
                close(sockfd);
                snprintf(s_status, sizeof(s_status), "TCP Client: Disconnected");
            }
        } else {
            snprintf(s_status, sizeof(s_status), "UDP: Local %d <-> %s:%d", config.local_port, config.remote_ip, config.remote_port);
            sockfd = socket(AF_INET, SOCK_DGRAM, 0);
            if (sockfd >= 0) {
                memset(&server_addr, 0, sizeof(server_addr));
                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(config.local_port);
                server_addr.sin_addr.s_addr = INADDR_ANY;
                if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                    bool udp_peer_valid = false;
                    memset(&client_addr, 0, sizeof(client_addr));
                    if (inet_pton(AF_INET, config.remote_ip, &client_addr.sin_addr) == 1 && config.remote_port > 0) {
                        client_addr.sin_family = AF_INET;
                        client_addr.sin_port = htons(config.remote_port);
                        udp_peer_valid = true;
                    }
                    passthrough_socket = sockfd;
                    bridge_socket_to_cdc(sockfd, true, &client_addr, &udp_peer_valid);
                    passthrough_socket = -1;
                }
                close(sockfd);
            }
        }
        
        if (passthrough_running) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    vTaskDelete(NULL);
}

void passthrough_init(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // STDIN, STDOUT, STDERR all share the same /dev/cdcacm fd in ESP-IDF.
    // cdcacm_fcntl(F_SETFL) toggles a global s_blocking flag:
    //   - O_NONBLOCK → s_blocking=false: read() returns EAGAIN when empty (polling loop works)
    //   - ~O_NONBLOCK → s_blocking=true:  read() blocks forever on semaphore (breaks bridge loop)
    // Therefore only set STDIN non-blocking; do NOT touch STDOUT to avoid
    // accidentally re-enabling blocking mode for the shared cdcacm fd.
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    // cdcacm_write() never checks s_blocking, so write() behaviour is
    // unaffected by the flag; fsync(STDOUT_FILENO) is used (not fflush) to
    // flush the USB CDC ROM TX buffer since fflush is a no-op on _IONBF streams.
}

esp_err_t passthrough_start(void) {
    passthrough_stop();
    passthrough_running = true;
    xTaskCreate(passthrough_task, "passthrough_task", 8192, NULL, 5, &passthrough_task_handle);
    return ESP_OK;
}

void passthrough_stop(void) {
    if (passthrough_running && passthrough_task_handle) {
        vTaskDelete(passthrough_task_handle);
        passthrough_task_handle = NULL;
    }
    if (passthrough_socket >= 0) {
        close(passthrough_socket);
        passthrough_socket = -1;
    }
    passthrough_running = false;
    strcpy(s_status, "Stopped");
}

bool passthrough_is_running(void) {
    return passthrough_running;
}
