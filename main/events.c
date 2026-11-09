/* Ring buffer of human readable events; the web page polls it. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app.h"

#define EVENT_COUNT 24

static const char *TAG = "event";
static app_event_t s_events[EVENT_COUNT];
static uint32_t s_last_id;
static SemaphoreHandle_t s_lock;

void events_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

void event_log(const char *fmt, ...)
{
    char msg[EVENT_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", msg);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    app_event_t *e = &s_events[++s_last_id % EVENT_COUNT];
    e->id = s_last_id;
    e->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    memcpy(e->msg, msg, sizeof(msg));
    xSemaphoreGive(s_lock);
}

int events_copy_after(uint32_t after_id, app_event_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t first = s_last_id >= EVENT_COUNT ? s_last_id - EVENT_COUNT + 1 : 1;
    if (after_id + 1 > first) {
        first = after_id + 1;
    }
    for (uint32_t id = first; id <= s_last_id && n < max; id++) {
        out[n++] = s_events[id % EVENT_COUNT];
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t events_last_id(void)
{
    return s_last_id;
}
