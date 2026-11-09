#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define MAX_DEVICES   64
#define MAX_ROOM      50
#define ROOM_ICON_MAX 8   /* illustrations offered by the web page */
#define ROOM_NAME_LEN 40
#define EVENT_MSG_LEN 96

typedef enum {
    DEV_UNKNOWN = 0,
    DEV_LAMP    = 1,   /* router-capable, mains powered */
    DEV_REMOTE  = 2,   /* sleepy end device, battery */
} device_type_t;

typedef struct {
    uint8_t  ieee[8];
    uint16_t short_addr;
    uint8_t  type;
    uint8_t  room;     /* 0 = not assigned */
} device_t;

/* Live lamp state from attribute reports (RAM only). */
typedef struct {
    int8_t   on;       /* -1 unknown, 0 off, 1 on */
    uint8_t  level;    /* 0 unknown, else 1..254 */
    uint16_t mireds;   /* 0 unknown */
} lamp_state_t;

/* What the web page shows for a room: settled values, or the commanded
 * target while the lamps are still getting there. -1 = not known. */
typedef struct {
    int8_t   on;
    int8_t   level_pct;
    int16_t  kelvin;
} room_view_t;

/* Night mode of a room: lights that stay bright inside [from, to) are dimmed
 * to the room's night percent (zb_snapshot_t.night_pct, default 1 %). */
typedef struct {
    uint8_t  enabled;
    uint8_t  from;     /* hour 0..23 */
    uint8_t  to;       /* hour 0..23 */
} night_cfg_t;

typedef struct {
    bool     ready;
    bool     pairing;
    int      pair_room;
    int      pair_left;         /* seconds */
    uint16_t pan_id;
    uint8_t  channel;
    int      device_count;
    device_t devices[MAX_DEVICES];
    lamp_state_t states[MAX_DEVICES];
    char     room_names[MAX_ROOM + 1][ROOM_NAME_LEN];
    night_cfg_t night[MAX_ROOM + 1];
    uint8_t  night_pct[MAX_ROOM + 1];    /* night mode dims to this percent */
    uint8_t  night_delay[MAX_ROOM + 1];  /* ... after the light was bright this many minutes */
    uint8_t  room_icons[MAX_ROOM + 1];   /* 0 = picked from the name */
    room_view_t views[MAX_ROOM + 1];
} zb_snapshot_t;

typedef struct {
    uint32_t id;
    uint32_t uptime_s;
    char     msg[EVENT_MSG_LEN];
} app_event_t;

/* ---- zigbee.c ----
 * Every room is one Zigbee group (group id = room number, 1..MAX_ROOM).
 * room 0 in the command functions means "all lamps".
 * All functions are safe to call from any task (they take the Zigbee lock). */
void        zb_start(void);
bool        zb_is_ready(void);
bool        zb_is_pairing(void);
esp_err_t   zb_onoff(int room, uint8_t zcl_cmd_id);
esp_err_t   zb_onoff_device(int index, uint8_t zcl_cmd_id);
esp_err_t   zb_level(int room, int percent);
esp_err_t   zb_temp(int room, int kelvin);
esp_err_t   zb_pair_start(int room, int seconds);
esp_err_t   zb_pair_stop(void);
esp_err_t   zb_assign(int index, int room);
esp_err_t   zb_remote_arrows(int index);
esp_err_t   zb_forget(int index);
esp_err_t   zb_rename_room(int room, const char *name);
esp_err_t   zb_set_night(int room, bool enabled, int from, int to, int pct, int delay_min);   /* pct 1..100, delay 1..120 min, -1 = keep */
esp_err_t   zb_set_room_icon(int room, int icon);   /* 0 = auto, 1..ROOM_ICON_MAX */
void        zb_netreset(void);
void        zb_get_snapshot(zb_snapshot_t *out);
const char *zb_type_name(uint8_t type);
void        zb_ieee_to_str(const uint8_t ieee[8], char *out, size_t len);

/* ---- events.c: small ring buffer the web page polls ---- */
void events_init(void);
void event_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  events_copy_after(uint32_t after_id, app_event_t *out, int max);
uint32_t events_last_id(void);

/* ---- web.c ---- */
void web_start(void);
bool wifi_has_credentials(void);
esp_err_t wifi_save_credentials(const char *ssid, const char *pass);
void wifi_clear_credentials(void);
const char *wifi_status_text(void);
bool wifi_is_setup_mode(void);   /* setup access point is open */
bool wifi_is_connected(void);    /* connected to home WiFi */
int  wifi_ap_clients(void);      /* phones on the setup network */

/* ---- clock.c: internet time + time zone ---- */
void        clock_start(void);
bool        clock_valid(void);
const char *clock_tz(void);
esp_err_t   clock_set_tz(const char *posix_tz);

/* ---- led.c: XIAO user LED shows the status ---- */
typedef enum { LED_BOOT_NONE, LED_BOOT_HOLDING, LED_BOOT_DONE } led_boot_t;
void led_start(void);
void led_set_boot(led_boot_t state);

/* ---- console.c ---- */
void console_start(void);
