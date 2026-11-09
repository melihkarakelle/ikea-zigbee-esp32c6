idf_component_register(SRCS "main.c" "zigbee.c" "events.c" "web.c" "console.c" "clock.c" "led.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash console esp_driver_gpio esp_wifi esp_netif
                                     esp_http_server esp_timer json lwip
                       EMBED_TXTFILES "web/index.html")
