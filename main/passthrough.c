#include "passthrough.h"
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
    fd_set readfds;
    struct timeval tv;

    while (passthrough_running) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sockfd, &readfds);
        int max_fd = (STDIN_FILENO > sockfd) ? STDIN_FILENO : sockfd;

        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50ms timeout

        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);

        if (activity < 0 && errno != EINTR) {
            ESP_LOGE("PASSTHROUGH", "Select error: %d", errno);
            break;
        }

        if (activity == 0) {
            continue;
        }

        // Data from CDC (Serial) -> Network
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            int len = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (len > 0) {
                web_server_ws_broadcast(buffer, len, WS_TYPE_SERIAL);
                int sent = 0;
                if (is_udp) {
                    if (*udp_peer_valid) {
                        sent = sendto(sockfd, buffer, len, 0, (struct sockaddr *)udp_peer, sizeof(*udp_peer));
                    }
                } else {
                    sent = send(sockfd, buffer, len, 0);
                }
                if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGE("PASSTHROUGH", "Socket send error: %d", errno);
                    break;
                }
            } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }

        // Data from Network -> CDC (Serial)
        if (FD_ISSET(sockfd, &readfds)) {
            int len = 0;
            if (is_udp) {
                struct sockaddr_in from_addr;
                socklen_t from_len = sizeof(from_addr);
                len = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &from_len);
                if (len > 0) {
                    *udp_peer = from_addr;
                    *udp_peer_valid = true;
                }
            } else {
                len = recv(sockfd, buffer, sizeof(buffer), 0);
            }

            if (len > 0) {
                web_server_ws_broadcast(buffer, len, WS_TYPE_NETWORK);
                
                // Write to STDOUT (mapped to USB CDC)
                int written = 0;
                int retry = 0;
                while (written < len && passthrough_running) {
                    int ret = write(STDOUT_FILENO, buffer + written, len - written);
                    if (ret > 0) {
                        written += ret;
                        retry = 0;
                    } else if (ret < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            vTaskDelay(pdMS_TO_TICKS(1));
                            if (++retry > 100) break; // Timeout after ~100ms
                            continue;
                        }
                        break;
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(1));
                        if (++retry > 100) break;
                    }
                }
            } else if (len == 0 || (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                break;
            }
        }
    }
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
                    listen(sockfd, 1);
                    client_len = sizeof(client_addr);
                    client_fd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd >= 0) {
                        char client_ip[16];
                        inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
                        snprintf(s_status, sizeof(s_status), "TCP Server: Connected to %s", client_ip);
                        passthrough_socket = client_fd;
                        bridge_socket_to_cdc(client_fd, false, NULL, NULL);
                        passthrough_socket = -1;
                        close(client_fd);
                        snprintf(s_status, sizeof(s_status), "TCP Server: Listening on %d", config.local_port);
                    }
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
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
}

esp_err_t passthrough_start(void) {
    passthrough_stop();
    passthrough_running = true;
    xTaskCreate(passthrough_task, "passthrough_task", 4096, NULL, 5, &passthrough_task_handle);
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
