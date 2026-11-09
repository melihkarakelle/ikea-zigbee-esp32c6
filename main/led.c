/*
 * Status LED: the XIAO ESP32-C6 user LED (GPIO15, lit when driven low).
 *
 * Patterns, highest priority first:
 *   BOOT held          solid on while counting the 5 s
 *   BOOT 5 s reached   fast blink 5 Hz until the restart
 *   setup mode         fast blink 5 Hz (setup network "IKEA-Zigbee-Kurulum" open)
 *   setup + phone      blink 2 Hz once a phone joins the setup network
 *   pairing open       blink 2 Hz
 *   starting up        slow blink 0.5 Hz (connecting to WiFi, Zigbee starting)
 *   running            one short flash every 2 s
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "app.h"

#define LED_GPIO   GPIO_NUM_15
#define TICK_MS    20

static volatile led_boot_t s_boot = LED_BOOT_NONE;

void led_set_boot(led_boot_t state)
{
    s_boot = state;
}

/* on/off segments in ms, starting with "on"; the pattern repeats */
typedef struct { const uint16_t *seg; int n; } pattern_t;

static const uint16_t SEG_5HZ[]   = { 100, 100 };
static const uint16_t SEG_2HZ[]   = { 250, 250 };
static const uint16_t SEG_05HZ[]  = { 1000, 1000 };
static const uint16_t SEG_BLIP[]  = { 60, 1940 };

#define PAT(a) ((pattern_t){ a, sizeof(a) / sizeof(a[0]) })

static bool pattern_level(pattern_t p, int64_t t_ms)
{
    int total = 0;
    for (int i = 0; i < p.n; i++) {
        total += p.seg[i];
    }
    int t = (int)(t_ms % total);
    for (int i = 0; i < p.n; i++) {
        if (t < p.seg[i]) {
            return (i % 2) == 0;
        }
        t -= p.seg[i];
    }
    return false;
}

static void led_task(void *pv)
{
    for (;;) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool on;
        if (s_boot == LED_BOOT_HOLDING) {
            on = true;
        } else if (s_boot == LED_BOOT_DONE) {
            on = pattern_level(PAT(SEG_5HZ), now_ms);
        } else if (wifi_is_setup_mode()) {
            on = pattern_level(wifi_ap_clients() > 0 ? PAT(SEG_2HZ) : PAT(SEG_5HZ), now_ms);
        } else if (zb_is_pairing()) {
            on = pattern_level(PAT(SEG_2HZ), now_ms);
        } else if (!wifi_is_connected() || !zb_is_ready()) {
            on = pattern_level(PAT(SEG_05HZ), now_ms);
        } else {
            on = pattern_level(PAT(SEG_BLIP), now_ms);
        }
        gpio_set_level(LED_GPIO, on ? 0 : 1);   /* active low */
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void led_start(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 1);   /* off until the task picks a pattern */
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
}
