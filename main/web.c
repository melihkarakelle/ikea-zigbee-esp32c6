/*
 * WiFi + web interface.
 *
 *  - WiFi credentials saved in NVS: join the home network as a station,
 *    reachable at http://ikea.local (mDNS) or its IP.
 *  - No credentials: open setup access point "IKEA-Zigbee-Kurulum" with a captive
 *    portal (every DNS name -> 192.168.4.1) that shows the setup page.
 *  - Holding the BOOT button for 5 s clears the credentials and restarts into setup.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_coexist.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "cJSON.h"
#include "esp_zigbee_core.h"
#include "app.h"
/* Optional home WiFi built into the firmware: copy wifi_secret.example.h to
 * wifi_secret.h (git-ignored). Without it the board starts in setup mode. */
#if __has_include("wifi_secret.h")
#include "wifi_secret.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

static const char *TAG = "web";

#define SETUP_AP_SSID    "IKEA-Zigbee-Kurulum"
#define MDNS_HOSTNAME    "ikea"
#define BOOT_BUTTON      GPIO_NUM_9
#define BOOT_HOLD_MS     5000
#define STA_RETRY_US     (5 * 1000000)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static char s_ssid[33];
static char s_ip[16];
static bool s_have_credentials;
static bool s_sta_connected;
static bool s_setup_mode;              /* setup access point is up */
static volatile int s_ap_clients;      /* phones joined to the setup network */
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_restart_timer;
static char s_status[96];

/* ---------- credentials ---------- */

static bool wifi_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    bool ok = false;
    uint8_t no_builtin = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t sl = ssid_len, pl = pass_len;
        ok = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK &&
             nvs_get_str(h, "pass", pass, &pl) == ESP_OK && ssid[0];
        nvs_get_u8(h, "nobuiltin", &no_builtin);
        nvs_close(h);
    }
    /* Fall back to the WiFi built into the firmware (wifi_secret.h),
     * but not after a WiFi reset (BOOT button): then the setup network opens. */
    if (!ok && !no_builtin && WIFI_PASS[0]) {
        snprintf(ssid, ssid_len, "%s", WIFI_SSID);
        snprintf(pass, pass_len, "%s", WIFI_PASS);
        ok = true;
    }
    return ok;
}

bool wifi_has_credentials(void)
{
    char ssid[33], pass[65];
    return wifi_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
}

esp_err_t wifi_save_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || !pass || strlen(pass) > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("wifi", NVS_READWRITE, &h), TAG, "nvs");
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void wifi_clear_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_set_u8(h, "nobuiltin", 1);   /* do not fall back to wifi_secret.h */
        nvs_commit(h);
        nvs_close(h);
    }
}

bool wifi_is_setup_mode(void)
{
    return s_setup_mode;
}

int wifi_ap_clients(void)
{
    return s_ap_clients;
}

bool wifi_is_connected(void)
{
    return s_sta_connected;
}

const char *wifi_status_text(void)
{
    if (!s_have_credentials) {
        snprintf(s_status, sizeof(s_status), "Kurulum modu: '%s' agina baglan, http://192.168.4.1", SETUP_AP_SSID);
    } else if (s_sta_connected) {
        snprintf(s_status, sizeof(s_status), "%s bagli: http://%s  (http://%s.local)", s_ssid, s_ip, MDNS_HOSTNAME);
    } else {
        snprintf(s_status, sizeof(s_status), "%s agina baglaniliyor...", s_ssid);
    }
    return s_status;
}

/* ---------- WiFi events ---------- */

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_credentials) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_connected) {
            event_log("WiFi baglantisi koptu, tekrar deneniyor");
        }
        s_sta_connected = false;
        s_ip[0] = 0;
        if (s_have_credentials) {
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, STA_RETRY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        event_log("WiFi bagli: http://%s  ya da  http://%s.local", s_ip, MDNS_HOSTNAME);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        event_log("Kurulum agina bir cihaz baglandi");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
    }
}

/* ---------- captive portal DNS (setup mode only) ---------- */

static void dns_task(void *pv)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &from_len);
        if (len < 12) {
            continue;
        }
        /* Answer every question with 192.168.4.1. */
        buf[2] = 0x81; buf[3] = 0x80;          /* response, recursion available */
        buf[6] = 0x00; buf[7] = 0x01;          /* one answer */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        const uint8_t answer[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
                                   0x00, 0x04, 192, 168, 4, 1 };
        memcpy(buf + len, answer, sizeof(answer));
        sendto(sock, buf, len + sizeof(answer), 0, (struct sockaddr *)&from, from_len);
    }
}

/* ---------- HTTP helpers ---------- */

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* Reads the query string (or the form body for POST with a body) into a malloc'd buffer. */
static char *get_params(httpd_req_t *req)
{
    if (req->method == HTTP_POST && req->content_len > 0 && req->content_len < 512) {
        char *body = calloc(1, req->content_len + 1);
        int got = 0;
        while (body && got < req->content_len) {
            int r = httpd_req_recv(req, body + got, req->content_len - got);
            if (r <= 0) {
                free(body);
                return NULL;
            }
            got += r;
        }
        return body;
    }
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) {
        return NULL;
    }
    char *q = calloc(1, len + 1);
    if (q && httpd_req_get_url_query_str(req, q, len + 1) != ESP_OK) {
        free(q);
        return NULL;
    }
    return q;
}

static bool param_str(const char *params, const char *key, char *out, size_t len)
{
    if (!params || httpd_query_key_value(params, key, out, len) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

static bool param_int(const char *params, const char *key, int *out)
{
    char buf[16];
    if (!param_str(params, key, buf, sizeof(buf))) {
        return false;
    }
    *out = atoi(buf);
    return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *txt = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt ? txt : "{}");
    free(txt);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, esp_err_t result)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", result == ESP_OK);
    if (result != ESP_OK) {
        const char *msg = result == ESP_ERR_INVALID_STATE ? "Zigbee agi hazir degil" :
                          result == ESP_ERR_INVALID_ARG   ? "Gecersiz deger" : esp_err_to_name(result);
        cJSON_AddStringToObject(json, "error", msg);
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return send_json(req, json);
}

/* ---------- HTTP handlers ---------- */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t state_handler(httpd_req_t *req)
{
    zb_snapshot_t *s = malloc(sizeof(zb_snapshot_t));
    if (!s) {
        return httpd_resp_send_500(req);
    }
    zb_get_snapshot(s);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", s->ready);
    cJSON_AddNumberToObject(root, "channel", s->channel);
    cJSON_AddNumberToObject(root, "pan", s->pan_id);
    cJSON_AddNumberToObject(root, "ev", events_last_id());
    if (clock_valid()) {
        char hm[8];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(hm, sizeof(hm), "%H:%M", &tm);
        cJSON_AddStringToObject(root, "time", hm);
    } else {
        cJSON_AddNullToObject(root, "time");
    }
    cJSON_AddStringToObject(root, "tz", clock_tz());

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "mode", s_have_credentials ? "sta" : "ap");
    cJSON_AddStringToObject(wifi, "ssid", s_have_credentials ? s_ssid : SETUP_AP_SSID);
    cJSON_AddBoolToObject(wifi, "connected", s_sta_connected);
    cJSON_AddStringToObject(wifi, "ip", s_ip);
    wifi_ap_record_t ap;
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
    }

    cJSON *pair = cJSON_AddObjectToObject(root, "pairing");
    cJSON_AddBoolToObject(pair, "open", s->pairing);
    cJSON_AddNumberToObject(pair, "room", s->pair_room);
    cJSON_AddNumberToObject(pair, "left", s->pair_left);

    cJSON *rooms = cJSON_AddArrayToObject(root, "rooms");
    for (int r = 1; r <= MAX_ROOM; r++) {
        int lamps = 0, remotes = 0;
        for (int i = 0; i < s->device_count; i++) {
            if (s->devices[i].room == r) {
                lamps += s->devices[i].type == DEV_LAMP;
                remotes += s->devices[i].type == DEV_REMOTE;
            }
        }
        if (!s->room_names[r][0] && !lamps && !remotes) {
            continue;
        }
        cJSON *room = cJSON_CreateObject();
        cJSON_AddNumberToObject(room, "id", r);
        cJSON_AddStringToObject(room, "name", s->room_names[r]);
        cJSON_AddNumberToObject(room, "icon", s->room_icons[r]);
        cJSON_AddNumberToObject(room, "lamps", lamps);
        cJSON_AddNumberToObject(room, "remotes", remotes);
        /* settled values or the commanded target; null = not known yet */
        const room_view_t *v = &s->views[r];
        if (v->on >= 0) {
            cJSON_AddBoolToObject(room, "on", v->on);
        } else {
            cJSON_AddNullToObject(room, "on");
        }
        if (v->level_pct >= 0) {
            cJSON_AddNumberToObject(room, "level", v->level_pct);
        } else {
            cJSON_AddNullToObject(room, "level");
        }
        if (v->kelvin > 0) {
            cJSON_AddNumberToObject(room, "kelvin", v->kelvin);
        } else {
            cJSON_AddNullToObject(room, "kelvin");
        }
        cJSON *night = cJSON_AddObjectToObject(room, "night");
        cJSON_AddBoolToObject(night, "on", s->night[r].enabled);
        cJSON_AddNumberToObject(night, "from", s->night[r].from);
        cJSON_AddNumberToObject(night, "to", s->night[r].to);
        cJSON_AddNumberToObject(night, "pct", s->night_pct[r]);
        cJSON_AddNumberToObject(night, "delay", s->night_delay[r]);
        cJSON_AddItemToArray(rooms, room);
    }

    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < s->device_count; i++) {
        char ieee[17], addr[8];
        zb_ieee_to_str(s->devices[i].ieee, ieee, sizeof(ieee));
        snprintf(addr, sizeof(addr), "0x%04x", s->devices[i].short_addr);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "i", i);
        cJSON_AddStringToObject(d, "type", s->devices[i].type == DEV_LAMP ? "lamp" :
                                           s->devices[i].type == DEV_REMOTE ? "remote" : "unknown");
        cJSON_AddNumberToObject(d, "room", s->devices[i].room);
        cJSON_AddStringToObject(d, "addr", addr);
        cJSON_AddStringToObject(d, "ieee", ieee);
        cJSON_AddItemToArray(devs, d);
    }
    free(s);
    return send_json(req, root);
}

static esp_err_t events_handler(httpd_req_t *req)
{
    char *params = get_params(req);
    int after = 0;
    param_int(params, "after", &after);
    free(params);

    app_event_t *evs = malloc(sizeof(app_event_t) * 24);
    if (!evs) {
        return httpd_resp_send_500(req);
    }
    int n = events_copy_after((uint32_t)after, evs, 24);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", evs[i].id);
        cJSON_AddNumberToObject(e, "t", evs[i].uptime_s);
        cJSON_AddStringToObject(e, "msg", evs[i].msg);
        cJSON_AddItemToArray(arr, e);
    }
    free(evs);
    return send_json(req, arr);
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *params = get_params(req);
    char action[12] = "";
    int room = -1, value = 0, dev = -1;
    param_str(params, "a", action, sizeof(action));
    param_int(params, "room", &room);
    param_int(params, "v", &value);
    param_int(params, "dev", &dev);
    free(params);

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (dev >= 0) {
        if (!strcmp(action, "on"))          err = zb_onoff_device(dev, ESP_ZB_ZCL_CMD_ON_OFF_ON_ID);
        else if (!strcmp(action, "off"))    err = zb_onoff_device(dev, ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID);
    } else if (room >= 0) {
        if (!strcmp(action, "on"))          err = zb_onoff(room, ESP_ZB_ZCL_CMD_ON_OFF_ON_ID);
        else if (!strcmp(action, "off"))    err = zb_onoff(room, ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID);
        else if (!strcmp(action, "toggle")) err = zb_onoff(room, ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID);
        else if (!strcmp(action, "dim"))    err = zb_level(room, value);
        else if (!strcmp(action, "temp"))   err = zb_temp(room, value);
    }
    return send_result(req, err);
}

static esp_err_t pair_handler(httpd_req_t *req)
{
    char *params = get_params(req);
    int room = 0, seconds = 180, stop = 0;
    param_int(params, "room", &room);
    param_int(params, "s", &seconds);
    param_int(params, "stop", &stop);
    free(params);
    return send_result(req, stop ? zb_pair_stop() : zb_pair_start(room, seconds));
}

static esp_err_t device_handler(httpd_req_t *req)
{
    char *params = get_params(req);
    int index = -1, room = 0, forget = 0, arrows = 0;
    param_int(params, "i", &index);
    param_int(params, "room", &room);
    param_int(params, "forget", &forget);
    param_int(params, "arrows", &arrows);
    free(params);
    if (arrows) {
        return send_result(req, zb_remote_arrows(index));
    }
    return send_result(req, forget ? zb_forget(index) : zb_assign(index, room));
}

static esp_err_t room_handler(httpd_req_t *req)
{
    char *params = get_params(req);
    int id = 0, icon = 0;
    char name[ROOM_NAME_LEN * 3] = "";
    param_int(params, "id", &id);
    bool has_name = param_str(params, "name", name, sizeof(name));
    bool has_icon = param_int(params, "icon", &icon);
    free(params);
    esp_err_t err = ESP_OK;
    if (has_icon) {
        err = zb_set_room_icon(id, icon);
    }
    if (err == ESP_OK && (has_name || !has_icon)) {
        err = zb_rename_room(id, name);
    }
    return send_result(req, err);
}

static esp_err_t night_handler(httpd_req_t *req)
{
    char *params = get_params(req);
    int id = 0, on = 1, from = 0, to = 4, pct = -1, delay = -1;
    param_int(params, "id", &id);
    param_int(params, "on", &on);
    param_int(params, "from", &from);
    param_int(params, "to", &to);
    param_int(params, "pct", &pct);
    param_int(params, "delay", &delay);
    free(params);
    return send_result(req, zb_set_night(id, on != 0, from, to, pct, delay));
}

static esp_err_t tz_handler(httpd_req_t *req)
{
    char *params = get_params(req);
    char tz[48 * 3] = "";
    param_str(params, "tz", tz, sizeof(tz));
    free(params);
    return send_result(req, clock_set_tz(tz));
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        return send_result(req, err);
    }
    uint16_t count = 20;
    wifi_ap_record_t *aps = calloc(count, sizeof(wifi_ap_record_t));
    if (!aps) {
        return httpd_resp_send_500(req);
    }
    esp_wifi_scan_get_ap_records(&count, aps);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        if (!aps[i].ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            dup |= !strcmp((char *)aps[j].ssid, (char *)aps[i].ssid);
        }
        if (dup) {
            continue;
        }
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid", (char *)aps[i].ssid);
        cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(a, "open", aps[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, a);
    }
    free(aps);
    return send_json(req, arr);
}

static esp_err_t wifi_handler(httpd_req_t *req)
{
    char *params = get_params(req);
    char ssid[33 * 3] = "", pass[64 * 3] = "";
    param_str(params, "ssid", ssid, sizeof(ssid));
    param_str(params, "pass", pass, sizeof(pass));
    free(params);
    esp_err_t err = wifi_save_credentials(ssid, pass);
    if (err == ESP_OK) {
        event_log("WiFi ayari kaydedildi (%s), yeniden baslatiliyor", ssid);
        esp_timer_start_once(s_restart_timer, 1500 * 1000);
    }
    return send_result(req, err);
}

/* Captive portal: any unknown URL goes to the main page. */
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t code)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_have_credentials ? "/" : "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/api/state",   .method = HTTP_GET,  .handler = state_handler },
        { .uri = "/api/events",  .method = HTTP_GET,  .handler = events_handler },
        { .uri = "/api/cmd",     .method = HTTP_POST, .handler = cmd_handler },
        { .uri = "/api/pair",    .method = HTTP_POST, .handler = pair_handler },
        { .uri = "/api/device",  .method = HTTP_POST, .handler = device_handler },
        { .uri = "/api/room",    .method = HTTP_POST, .handler = room_handler },
        { .uri = "/api/night",   .method = HTTP_POST, .handler = night_handler },
        { .uri = "/api/tz",      .method = HTTP_POST, .handler = tz_handler },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = scan_handler },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = wifi_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);
}

/* ---------- BOOT button: hold 5 s to forget WiFi ---------- */

static void button_task(void *pv)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held_ms = gpio_get_level(BOOT_BUTTON) == 0 ? held_ms + 100 : 0;
        /* LED flickers while the button is held, so a short press is visible too */
        led_set_boot(held_ms >= 300 ? LED_BOOT_HOLDING : LED_BOOT_NONE);
        if (held_ms >= BOOT_HOLD_MS) {
            led_set_boot(LED_BOOT_DONE);   /* solid on; stays on in setup mode */
            event_log("BOOT tusu: WiFi ayari siliniyor, kurulum moduna geciliyor");
            wifi_clear_credentials();
            /* restart once the button is let go (at most 10 s), LED stays on meanwhile */
            for (int i = 0; i < 100 && gpio_get_level(BOOT_BUTTON) == 0; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}

/* ---------- start ---------- */

void web_start(void)
{
    char pass[65] = "";
    s_have_credentials = wifi_load_credentials(s_ssid, sizeof(s_ssid), pass, sizeof(pass));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, MDNS_HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    const esp_timer_create_args_t retry_args = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    const esp_timer_create_args_t restart_args = { .callback = restart_cb, .name = "restart" };
    ESP_ERROR_CHECK(esp_timer_create(&restart_args, &s_restart_timer));

    if (s_have_credentials) {
        wifi_config_t sta_cfg = {0};
        /* ssid (32) and password (64) are not NUL-terminated when full. */
        memcpy(sta_cfg.sta.ssid, s_ssid, strnlen(s_ssid, sizeof(sta_cfg.sta.ssid)));
        memcpy(sta_cfg.sta.password, pass, strnlen(pass, sizeof(sta_cfg.sta.password)));
        sta_cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        event_log("WiFi: %s agina baglaniliyor", s_ssid);
    } else {
        esp_netif_create_default_wifi_ap();
        wifi_config_t ap_cfg = {
            .ap = {
                .ssid = SETUP_AP_SSID,
                .ssid_len = strlen(SETUP_AP_SSID),
                .channel = 6,
                .authmode = WIFI_AUTH_OPEN,
                .max_connection = 2,
            },
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));   /* STA part is used for scanning */
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
        s_setup_mode = true;
        event_log("Kurulum modu: '%s' agina baglan, http://192.168.4.1", SETUP_AP_SSID);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (s_have_credentials) {
        /* WiFi and the 802.15.4 (Zigbee) radio share one RF: turn on time sharing,
         * otherwise Zigbee keeps the radio and WiFi cannot even scan.
         * Modem sleep is required for this coexistence. */
        ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        clock_start();
    } else {
        /* SoftAP misses client frames with power save on; Zigbee is off in setup mode.
         * 20 MHz is the most compatible bandwidth for phones. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    }

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("IKEA Zigbee");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    http_start();
    xTaskCreate(button_task, "boot_btn", 3072, NULL, 3, NULL);
}
