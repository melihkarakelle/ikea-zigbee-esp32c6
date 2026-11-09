/*
 * IKEA TRÅDFRI Zigbee controller for Seeed XIAO ESP32-C6.
 * Zigbee coordinator (zigbee.c) + WiFi web page (web.c) + USB serial console (console.c)
 * + status LED (led.c).
 */

#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "app.h"

/* XIAO ESP32-C6 RF switch: GPIO3 low enables it, GPIO14 low = on-board antenna. */
#define XIAO_RF_SWITCH_EN    GPIO_NUM_3
#define XIAO_ANTENNA_SELECT  GPIO_NUM_14

static void xiao_antenna_init(void)
{
    gpio_reset_pin(XIAO_RF_SWITCH_EN);
    gpio_set_direction(XIAO_RF_SWITCH_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(XIAO_RF_SWITCH_EN, 0);
    gpio_reset_pin(XIAO_ANTENNA_SELECT);
    gpio_set_direction(XIAO_ANTENNA_SELECT, GPIO_MODE_OUTPUT);
    gpio_set_level(XIAO_ANTENNA_SELECT, 0);   /* 1 = external U.FL antenna */
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    xiao_antenna_init();
    led_start();
    events_init();
    /* The setup access point and the 802.15.4 radio cannot share the radio
     * reliably, so Zigbee only starts once home WiFi credentials exist. */
    if (wifi_has_credentials()) {
        zb_start();
    } else {
        event_log("Zigbee, WiFi ayarlandiktan sonra baslayacak");
    }
    web_start();
    console_start();
}
