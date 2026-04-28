idf_component_register(SRCS "main.c" 
                    "app_config.c" 
                    "wifi_manager.c" 
                    "led_ctrl.c" 
                    "passthrough.c" 
                    "web_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_server lwip driver vfs esp_hw_support
                    EMBED_FILES "index.html")
