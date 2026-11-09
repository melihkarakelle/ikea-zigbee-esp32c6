/*
 * Wall clock: time from the internet (SNTP), local time zone kept in NVS.
 * The web page sends the browser's time zone as a POSIX TZ string.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "app.h"

static const char *TAG = "clock";

#define DEFAULT_TZ "GMT0BST,M3.5.0/1,M10.5.0"   /* Europe/London */

static char s_tz[48] = DEFAULT_TZ;
static bool s_synced_logged;

static void time_sync_cb(struct timeval *tv)
{
    if (!s_synced_logged) {
        char buf[24];
        time_t now = tv->tv_sec;
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", &tm);
        event_log("Saat internetten alindi: %s", buf);
        s_synced_logged = true;
    }
}

void clock_start(void)
{
    nvs_handle_t h;
    if (nvs_open("clock", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_tz);
        if (nvs_get_str(h, "tz", s_tz, &len) != ESP_OK) {
            snprintf(s_tz, sizeof(s_tz), "%s", DEFAULT_TZ);
        }
        nvs_close(h);
    }
    setenv("TZ", s_tz, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = time_sync_cb;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed");
    }
}

bool clock_valid(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year >= (2025 - 1900);
}

const char *clock_tz(void)
{
    return s_tz;
}

esp_err_t clock_set_tz(const char *tz)
{
    size_t len = tz ? strlen(tz) : 0;
    if (len == 0 || len >= sizeof(s_tz)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isprint((unsigned char)tz[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (strcmp(tz, s_tz) == 0) {
        return ESP_OK;
    }
    snprintf(s_tz, sizeof(s_tz), "%s", tz);
    nvs_handle_t h;
    if (nvs_open("clock", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "tz", s_tz);
        nvs_commit(h);
        nvs_close(h);
    }
    setenv("TZ", s_tz, 1);
    tzset();
    event_log("Saat dilimi ayarlandi: %s", s_tz);
    return ESP_OK;
}
