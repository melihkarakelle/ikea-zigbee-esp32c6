/*
 * Zigbee coordinator core for IKEA TRÅDFRI.
 *
 * Every room is one Zigbee group (group id = room number).
 *  - Lamps (routers) that join while pairing is open are added to that room's group.
 *  - Remotes (end devices) that join are bound (OnOff, Level, Scenes) to that group,
 *    so a remote talks to its lamps directly and keeps working when this board is off.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_zigbee_core.h"
#include "aps/esp_zigbee_aps.h"
#include "test/esp_zigbee_test_utils.h"   /* esp_zb_aps_group_table_add_group */
#include "app.h"

static const char *TAG = "zigbee";

#define APP_ENDPOINT         1
#define IKEA_ENDPOINT        1        /* TRÅDFRI lamps and remotes use endpoint 1 */
#define MAX_CHILDREN         16
#define TRANSITION_DS        5        /* transition time in 1/10 s */
/* Channels 15, 20 and 25 sit between the common 2.4 GHz WiFi channels. */
#define ZB_CHANNEL_MASK      ((1UL << 15) | (1UL << 20) | (1UL << 25))

static device_t s_devices[MAX_DEVICES];
static uint8_t  s_device_count;
static char     s_room_names[MAX_ROOM + 1][ROOM_NAME_LEN];
static uint8_t  s_pair_room;           /* room for devices that join while the network is open */
static bool     s_network_open;
static int64_t  s_pair_until_us;
static volatile bool s_zb_ready;
static lamp_state_t s_state[MAX_DEVICES];   /* index-aligned with s_devices */
static bool     s_report_set[MAX_DEVICES];

/* Room view for the web page (see room_view_update) */
#define PENDING_US        (10LL * 1000000)   /* give lamps this long to reach a commanded value */
#define SETTLE_US         (1500LL * 1000)    /* a room's reports must be quiet this long */
typedef struct {
    int64_t  until_us;     /* 0 = nothing pending */
    int8_t   on;           /* -1 = not part of the command */
    int16_t  level;        /* raw 1..254, -1 = not part */
    int32_t  mireds;       /* -1 = not part */
} room_pending_t;
static room_pending_t s_pending[MAX_ROOM + 1];
static int64_t  s_room_last_report[MAX_ROOM + 1];
static room_view_t s_room_view[MAX_ROOM + 1];

/* Night mode */
#define NIGHT_DELAY_DEFAULT 5                    /* minutes bright inside the window -> dim */
#define NIGHT_PCT_DEFAULT 1                      /* dim to 1 % unless the room says otherwise */
#define NIGHT_MARGIN      3                      /* above target + this a room counts as bright */
#define NIGHT_TICK_MS     10000
static night_cfg_t s_night[MAX_ROOM + 1];
static uint8_t s_night_pct[MAX_ROOM + 1];   /* night mode target, percent */
static uint8_t s_night_delay[MAX_ROOM + 1]; /* minutes before dimming */
static uint8_t s_room_icon[MAX_ROOM + 1];   /* web page illustration, 0 = auto */
static int64_t  s_bright_since[MAX_ROOM + 1];   /* esp_timer us; reset when lights go on / brighter */
static bool     s_in_window[MAX_ROOM + 1];

static void remember_room_temp(int room, int kelvin);
static void reporting_chain_cb(uint8_t from);
static void night_tick_cb(uint8_t unused);
static void room_pending_set(int room, int on, int level, int mireds);

/* ---------- persistence ---------- */

static void devices_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ikea", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed");
        return;
    }
    nvs_set_u8(h, "count", s_device_count);
    if (s_device_count) {
        nvs_set_blob(h, "devs", s_devices, sizeof(device_t) * s_device_count);
    } else {
        nvs_erase_key(h, "devs");
    }
    nvs_commit(h);
    nvs_close(h);
}

static void rooms_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ikea", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "rooms", s_room_names, sizeof(s_room_names));
        nvs_set_blob(h, "icons", s_room_icon, sizeof(s_room_icon));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void night_save(void)
{
    nvs_handle_t h;
    if (nvs_open("ikea", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "night", s_night, sizeof(s_night));
        nvs_set_blob(h, "nightpct", s_night_pct, sizeof(s_night_pct));
        nvs_set_blob(h, "nightdly", s_night_delay, sizeof(s_night_delay));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void storage_load(void)
{
    for (int r = 0; r <= MAX_ROOM; r++) {
        s_night[r] = (night_cfg_t){ .enabled = 1, .from = 0, .to = 4 };
        s_night_pct[r] = NIGHT_PCT_DEFAULT;
        s_night_delay[r] = NIGHT_DELAY_DEFAULT;
    }
    nvs_handle_t h;
    if (nvs_open("ikea", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) == ESP_OK && count <= MAX_DEVICES) {
        size_t len = sizeof(device_t) * count;
        if (count == 0 || nvs_get_blob(h, "devs", s_devices, &len) == ESP_OK) {
            s_device_count = count;
        }
    }
    size_t len = sizeof(s_room_names);
    if (nvs_get_blob(h, "rooms", s_room_names, &len) != ESP_OK) {
        memset(s_room_names, 0, sizeof(s_room_names));
    }
    len = sizeof(s_room_icon);
    if (nvs_get_blob(h, "icons", s_room_icon, &len) != ESP_OK) {
        memset(s_room_icon, 0, sizeof(s_room_icon));
    }
    len = sizeof(s_night_pct);
    if (nvs_get_blob(h, "nightpct", s_night_pct, &len) != ESP_OK) {
        memset(s_night_pct, NIGHT_PCT_DEFAULT, sizeof(s_night_pct));
    }
    len = sizeof(s_night_delay);
    if (nvs_get_blob(h, "nightdly", s_night_delay, &len) != ESP_OK) {
        memset(s_night_delay, NIGHT_DELAY_DEFAULT, sizeof(s_night_delay));
    }
    len = sizeof(s_night);
    if (nvs_get_blob(h, "night", s_night, &len) != ESP_OK) {
        for (int r = 0; r <= MAX_ROOM; r++) {
            s_night[r] = (night_cfg_t){ .enabled = 1, .from = 0, .to = 4 };   /* default 00:00-04:00 */
        }
    }
    nvs_close(h);
    for (int i = 0; i < MAX_DEVICES; i++) {
        s_state[i] = (lamp_state_t){ .on = -1 };
    }
    for (int r = 0; r <= MAX_ROOM; r++) {
        s_room_view[r] = (room_view_t){ .on = -1, .level_pct = -1, .kelvin = -1 };
    }
    ESP_LOGI(TAG, "Loaded %d devices from NVS", s_device_count);
}

/* ---------- helpers ---------- */

const char *zb_type_name(uint8_t type)
{
    switch (type) {
    case DEV_LAMP:   return "lamba";
    case DEV_REMOTE: return "kumanda";
    default:         return "?";
    }
}

void zb_ieee_to_str(const uint8_t ieee[8], char *out, size_t len)
{
    /* esp_zb_ieee_addr_t is little endian; print it MSB first. */
    snprintf(out, len, "%02x%02x%02x%02x%02x%02x%02x%02x",
             ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);
}

static const char *room_label(int room, char *buf, size_t len)
{
    if (room >= 1 && room <= MAX_ROOM && s_room_names[room][0]) {
        snprintf(buf, len, "%s", s_room_names[room]);
    } else {
        snprintf(buf, len, "Oda %d", room);
    }
    return buf;
}

static int device_find(const uint8_t ieee[8])
{
    for (int i = 0; i < s_device_count; i++) {
        if (memcmp(s_devices[i].ieee, ieee, 8) == 0) {
            return i;
        }
    }
    return -1;
}

/* ---------- group assignment (Zigbee context) ---------- */

static void lamp_add_to_group(const device_t *dev, uint8_t room)
{
    esp_zb_zcl_groups_add_group_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.src_endpoint = APP_ENDPOINT;
    cmd.zcl_basic_cmd.dst_endpoint = IKEA_ENDPOINT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = dev->short_addr;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.group_id = room;
    esp_zb_zcl_groups_add_group_cmd_req(&cmd);
}

static void lamp_remove_from_group(const device_t *dev, uint8_t room)
{
    esp_zb_zcl_groups_add_group_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.src_endpoint = APP_ENDPOINT;
    cmd.zcl_basic_cmd.dst_endpoint = IKEA_ENDPOINT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = dev->short_addr;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.group_id = room;
    esp_zb_zcl_groups_remove_group_cmd_req(&cmd);
}

/* ---------- remote binding (Zigbee context) ----------
 * On/Off and Level are bound to the room's group, so the remote talks to its
 * lamps directly and keeps working with this board off. Newer E1810 firmware
 * refuses group bindings (NOT_SUPPORTED); then every lamp of the room is bound
 * one by one instead.
 * The arrows (Scenes cluster) are bound to this board, which turns them into
 * colour temperature commands: group frames from a remote never reach the
 * coordinator, unicast ones do.
 * A remote only hears requests while awake, so everything is retried every
 * 3 s for 30 s; pressing any button on the remote wakes it up. */

#define ZDP_NOT_SUPPORTED 0x84
#define ZDP_TABLE_FULL    0x8c

static int      s_rb_idx = -1;     /* remote being bound, -1 = none */
static uint8_t  s_rb_tries;
static bool     s_rb_light_ok;     /* On/Off + Level bound */
static bool     s_rb_arrows_ok;
static bool     s_rb_per_lamp;     /* group refused: bind lamps one by one */
static uint64_t s_rb_lamp_mask;    /* lamps whose On/Off bind is confirmed */

static void remote_unbind_group(const device_t *dev, uint8_t room)
{
    static const uint16_t clusters[] = { ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                                         ESP_ZB_ZCL_CLUSTER_ID_SCENES };
    for (size_t i = 0; i < sizeof(clusters) / sizeof(clusters[0]); i++) {
        esp_zb_zdo_bind_req_param_t req = {0};
        memcpy(req.src_address, dev->ieee, sizeof(esp_zb_ieee_addr_t));
        req.src_endp = IKEA_ENDPOINT;
        req.cluster_id = clusters[i];
        req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_16_BIT_GROUP;
        req.dst_address_u.addr_short = room;
        req.req_dst_addr = dev->short_addr;
        esp_zb_zdo_device_unbind_req(&req, NULL, NULL);
    }
}

static void bind_req(const device_t *dev, uint16_t cluster, uint16_t group,
                     const uint8_t *dst_ieee, esp_zb_zdo_bind_callback_t cb, uintptr_t ctx)
{
    esp_zb_zdo_bind_req_param_t req = {0};
    memcpy(req.src_address, dev->ieee, sizeof(esp_zb_ieee_addr_t));
    req.src_endp = IKEA_ENDPOINT;
    req.cluster_id = cluster;
    req.req_dst_addr = dev->short_addr;
    if (dst_ieee == NULL) {
        req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_16_BIT_GROUP;
        req.dst_address_u.addr_short = group;
    } else {
        req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
        memcpy(req.dst_address_u.addr_long, dst_ieee, sizeof(esp_zb_ieee_addr_t));
        req.dst_endp = group;   /* endpoint when binding to a device */
    }
    esp_zb_zdo_device_bind_req(&req, cb, (void *)ctx);
}

static void group_bind_cb(esp_zb_zdp_status_t status, void *user_ctx)
{
    if ((uintptr_t)user_ctx != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        return;
    }
    ESP_LOGI(TAG, "Group bind status 0x%02x", status);
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        if (!s_rb_light_ok) {
            event_log("Kumanda odanin grubuna baglandi");
        }
        s_rb_light_ok = true;
    } else if (status == ZDP_NOT_SUPPORTED) {
        if (!s_rb_per_lamp) {
            event_log("Kumanda grubu desteklemiyor, lambalara tek tek baglaniyor");
        }
        s_rb_per_lamp = true;
    } else if (status != ESP_ZB_ZDP_STATUS_TIMEOUT) {
        event_log("Kumanda gruba baglanamadi: durum 0x%02x", status);
    }
}

static int room_lamp_count(uint8_t room)
{
    int n = 0;
    for (int i = 0; i < s_device_count; i++) {
        n += s_devices[i].type == DEV_LAMP && s_devices[i].room == room;
    }
    return n;
}

static void lamp_bind_cb(esp_zb_zdp_status_t status, void *user_ctx)
{
    uintptr_t ctx = (uintptr_t)user_ctx;
    int lamp = ctx & 0xFF;
    if ((ctx >> 8) != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF || s_rb_idx < 0) {
        return;
    }
    ESP_LOGI(TAG, "Lamp bind #%d status 0x%02x", lamp, status);
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        s_rb_lamp_mask |= 1ULL << lamp;
        int done = 0;
        for (int i = 0; i < s_device_count; i++) {
            done += (s_rb_lamp_mask >> i) & 1;
        }
        if (!s_rb_light_ok && done >= room_lamp_count(s_devices[s_rb_idx].room)) {
            s_rb_light_ok = true;
            event_log("Kumanda %d lambaya tek tek baglandi", done);
        }
    } else if (status != ESP_ZB_ZDP_STATUS_TIMEOUT) {
        event_log("Kumanda lamba #%d'e baglanamadi: durum 0x%02x%s", lamp, status,
                  status == ZDP_TABLE_FULL ? " (kumandanin baglanti tablosu dolu)" : "");
    }
}

static void arrows_bind_cb(esp_zb_zdp_status_t status, void *user_ctx)
{
    ESP_LOGI(TAG, "Arrow bind status 0x%02x", status);
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        if (!s_rb_arrows_ok) {
            event_log("Kumandanin ok tuslari ESP'ye baglandi");
        }
        s_rb_arrows_ok = true;
    } else if (status != ESP_ZB_ZDP_STATUS_TIMEOUT && !s_rb_arrows_ok) {
        event_log("Kumanda ok tuslarini baglamayi reddetti: durum 0x%02x", status);
    }
}

static void rebind_round(const device_t *dev)
{
    uint8_t room = dev->room;
    if (!s_rb_light_ok) {
        if (!s_rb_per_lamp) {
            bind_req(dev, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, room, NULL, group_bind_cb, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
            bind_req(dev, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, room, NULL, group_bind_cb, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL);
        } else {
            int sent = 0;
            for (int i = 0; i < s_device_count && i < 64; i++) {
                const device_t *lamp = &s_devices[i];
                if (lamp->type != DEV_LAMP || lamp->room != room || ((s_rb_lamp_mask >> i) & 1)) {
                    continue;
                }
                bind_req(dev, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, IKEA_ENDPOINT, lamp->ieee, lamp_bind_cb,
                         ((uintptr_t)ESP_ZB_ZCL_CLUSTER_ID_ON_OFF << 8) | i);
                bind_req(dev, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, IKEA_ENDPOINT, lamp->ieee, lamp_bind_cb,
                         ((uintptr_t)ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL << 8) | i);
                if (++sent >= 3) {
                    break;   /* a few per round: the parent only buffers a handful for a sleeping remote */
                }
            }
            if (room_lamp_count(room) == 0) {
                s_rb_light_ok = true;
            }
        }
    }
    if (!s_rb_arrows_ok) {
        esp_zb_ieee_addr_t me;
        esp_zb_get_long_address(me);
        bind_req(dev, ESP_ZB_ZCL_CLUSTER_ID_SCENES, APP_ENDPOINT, me, arrows_bind_cb, 0);
    }
}

static void rebind_retry_cb(uint8_t unused)
{
    if (s_rb_idx < 0 || s_rb_idx >= s_device_count) {
        s_rb_idx = -1;
        return;
    }
    if (s_rb_light_ok && s_rb_arrows_ok) {
        event_log("Kumanda #%d hazir", s_rb_idx);
        s_rb_idx = -1;
        return;
    }
    rebind_round(&s_devices[s_rb_idx]);
    if (++s_rb_tries < 12) {
        esp_zb_scheduler_alarm((esp_zb_callback_t)rebind_retry_cb, 0, 3000);
    } else {
        event_log("Kumanda #%d tam baglanamadi (uyanmadi). 'Yeniden bagla'ya basip kumandanin bir tusuna bas.", s_rb_idx);
        s_rb_idx = -1;
    }
}

static void rebind_start(int idx)
{
    bool running = s_rb_idx >= 0;
    s_rb_idx = idx;
    s_rb_tries = 0;
    s_rb_light_ok = false;
    s_rb_arrows_ok = false;
    s_rb_per_lamp = false;
    s_rb_lamp_mask = 0;
    if (!running) {
        rebind_retry_cb(0);
    }
}

static void device_assign_locked(int idx, uint8_t room)
{
    device_t *dev = &s_devices[idx];
    char label[ROOM_NAME_LEN + 8];
    room_label(room, label, sizeof(label));
    uint8_t old_room = dev->room;

    if (dev->type == DEV_LAMP) {
        if (old_room && old_room != room) {
            lamp_remove_from_group(dev, old_room);
        }
        lamp_add_to_group(dev, room);
        event_log("Lamba #%d -> %s", idx, label);
    } else if (dev->type == DEV_REMOTE) {
        if (old_room && old_room != room) {
            remote_unbind_group(dev, old_room);
        }
        dev->room = room;
        rebind_start(idx);
        event_log("Kumanda #%d -> %s (baglaniyor...)", idx, label);
    } else {
        return;
    }
    dev->room = room;
    devices_save();
}

/* A lamp ignores the add-group command right after joining (it is still
 * settling into the network), so it is sent at 3 s and repeated at 8 s.
 * A repeat is harmless: the lamp answers "already a member" (0x8a). */
static void lamp_group_cb(uint8_t idx)
{
    if (idx < s_device_count && s_devices[idx].type == DEV_LAMP && s_devices[idx].room) {
        lamp_add_to_group(&s_devices[idx], s_devices[idx].room);
    }
}

/* Remotes sleep soon after joining, so they are bound quickly (1 s). */
static void remote_bind_cb(uint8_t idx)
{
    if (idx < s_device_count && s_devices[idx].type == DEV_REMOTE && s_devices[idx].room) {
        rebind_start(idx);
    }
}

static void on_device_announce(const esp_zb_zdo_signal_device_annce_params_t *p)
{
    int idx = device_find(p->ieee_addr);
    bool is_new = idx < 0;
    if (is_new) {
        if (s_device_count >= MAX_DEVICES) {
            event_log("Cihaz tablosu dolu");
            return;
        }
        idx = s_device_count++;
        memset(&s_devices[idx], 0, sizeof(device_t));
        memcpy(s_devices[idx].ieee, p->ieee_addr, 8);
    }
    device_t *dev = &s_devices[idx];
    dev->short_addr = p->device_short_addr;
    /* MAC capability bit 1: full function device (router) => lamp. */
    dev->type = (p->capability & 0x02) ? DEV_LAMP : DEV_REMOTE;
    devices_save();
    s_report_set[idx] = false;
    if (is_new) {
        s_state[idx] = (lamp_state_t){ .on = -1 };
    }

    if (is_new || s_network_open) {
        event_log("%s %s katildi (#%d)", is_new ? "Yeni" : "Tekrar", zb_type_name(dev->type), idx);
    }
    /* Only touch groups/bindings while a pairing window is open. The room is
     * stored now, so closing the window early does not lose it. */
    if (s_network_open && s_pair_room) {
        char label[ROOM_NAME_LEN + 8];
        room_label(s_pair_room, label, sizeof(label));
        dev->room = s_pair_room;
        devices_save();
        if (dev->type == DEV_LAMP) {
            event_log("Lamba #%d -> %s", idx, label);
            esp_zb_scheduler_alarm((esp_zb_callback_t)lamp_group_cb, (uint8_t)idx, 3000);
            esp_zb_scheduler_alarm((esp_zb_callback_t)lamp_group_cb, (uint8_t)idx, 8000);
        } else {
            event_log("Kumanda #%d -> %s (baglaniyor...)", idx, label);
            esp_zb_scheduler_alarm((esp_zb_callback_t)remote_bind_cb, (uint8_t)idx, 1000);
        }
    }
}

/* ---------- Zigbee stack ---------- */

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, ,
                        TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = *p_sg_p;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Zigbee start failed: %s", esp_err_to_name(err));
            break;
        }
        if (esp_zb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "Forming a new network");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            s_zb_ready = true;
            event_log("Zigbee agi hazir (kanal %d)", esp_zb_get_current_channel());
            esp_zb_scheduler_alarm((esp_zb_callback_t)reporting_chain_cb, 0, 3000);
            esp_zb_scheduler_alarm((esp_zb_callback_t)night_tick_cb, 0, NIGHT_TICK_MS);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err == ESP_OK) {
            s_zb_ready = true;
            event_log("Yeni Zigbee agi kuruldu (kanal %d)", esp_zb_get_current_channel());
            esp_zb_scheduler_alarm((esp_zb_callback_t)night_tick_cb, 0, NIGHT_TICK_MS);
        } else {
            ESP_LOGW(TAG, "Formation failed (%s), retrying", esp_err_to_name(err));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        on_device_announce((esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p));
        break;
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err == ESP_OK) {
            uint8_t seconds = *(uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
            bool was_open = s_network_open;
            s_network_open = seconds > 0;
            if (seconds) {
                s_pair_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
            } else if (was_open) {
                s_pair_room = 0;
                event_log("Eslesme kapandi");
            }
        }
        break;
    default:
        ESP_LOGD(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig), sig, esp_err_to_name(err));
        break;
    }
}

static esp_zb_ep_list_t *create_endpoint(void)
{
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_cluster_list_add_basic_cluster(cl, esp_zb_basic_cluster_create(&basic_cfg),
                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(cl, esp_zb_identify_cluster_create(&identify_cfg),
                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Client clusters: this endpoint sends commands to the lamps. */
    esp_zb_cluster_list_add_on_off_cluster(cl, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF),
                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_level_cluster(cl, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL),
                                          ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_color_control_cluster(cl, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL),
                                                  ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_groups_cluster(cl, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_GROUPS),
                                           ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = APP_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_COLOR_DIMMER_SWITCH_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cl, ep_cfg);
    return ep_list;
}

/* ---------- remote arrow keys -> colour temperature ----------
 * IKEA remotes send their left/right arrows as IKEA-specific Scenes cluster
 * commands (0x07 click, 0x08 hold, 0x09 release). Lamps act on them only when
 * paired directly, not through a group binding. The arrows are bound to this
 * board (see rebind_round), which sends a standard "move to colour
 * temperature" command to the remote's room instead. */

static const uint16_t s_temp_presets[] = { 2200, 2700, 4000 };   /* warm, neutral, cold */
#define TEMP_PRESET_COUNT (sizeof(s_temp_presets) / sizeof(s_temp_presets[0]))
static uint8_t s_room_temp_idx[MAX_ROOM + 1];   /* 0 = unknown, else preset index + 1 */

static void room_temp_send(int room, int kelvin)
{
    esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.src_endpoint = APP_ENDPOINT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = (uint16_t)room;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_GROUP_ENDP_NOT_PRESENT;
    cmd.color_temperature = (uint16_t)(1000000 / kelvin);
    cmd.transition_time = TRANSITION_DS;
    esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
}

static void remember_room_temp(int room, int kelvin)
{
    if (room < 1 || room > MAX_ROOM) {
        return;
    }
    int best = 0;
    for (int i = 1; i < (int)TEMP_PRESET_COUNT; i++) {
        if (abs(kelvin - s_temp_presets[i]) < abs(kelvin - s_temp_presets[best])) {
            best = i;
        }
    }
    s_room_temp_idx[room] = best + 1;
}

static bool aps_indication_handler(esp_zb_apsde_data_ind_t ind)
{
    char hex[3 * 16 + 1] = "";
    for (uint32_t i = 0; i < ind.asdu_length && i < 16; i++) {
        snprintf(hex + 3 * i, 4, "%02x ", ind.asdu[i]);
    }
    ESP_LOGD(TAG, "APS ind: st %d from 0x%04hx ep %d -> mode %d addr 0x%04hx ep %d cluster 0x%04x len %lu: %s",
             ind.status, ind.src_short_addr, ind.src_endpoint, ind.dst_addr_mode, ind.dst_short_addr,
             ind.dst_endpoint, ind.cluster_id, (unsigned long)ind.asdu_length, hex);
    if (ind.status != 0 || ind.cluster_id != ESP_ZB_ZCL_CLUSTER_ID_SCENES || ind.asdu_length < 3) {
        return false;
    }
    const uint8_t *p = ind.asdu;
    uint8_t fc = p[0];
    size_t pos = (fc & 0x04) ? 3 : 1;   /* skip manufacturer code if present */
    if (ind.asdu_length < pos + 2) {
        return false;
    }
    uint8_t cmd_id = p[pos + 1];
    const uint8_t *payload = p + pos + 2;
    size_t payload_len = ind.asdu_length - (pos + 2);
    int room = 0;
    if (ind.dst_addr_mode == 0x01) {          /* group frame: the group is the room */
        room = ind.dst_short_addr;
    } else {                                  /* bound to this board: the remote's room */
        for (int i = 0; i < s_device_count; i++) {
            if (s_devices[i].short_addr == ind.src_short_addr) {
                room = s_devices[i].room;
            }
        }
    }

    if (cmd_id == 0x09) {
        return true;                    /* arrow released: nothing to do */
    }
    if ((cmd_id != 0x07 && cmd_id != 0x08) || payload_len < 1 || room < 1 || room > MAX_ROOM) {
        return false;
    }
    bool left = (payload[0] & 0x01) != 0;   /* 0x0101/0x0D01 = left, 0x0100/0x0D00 = right */
    int idx = s_room_temp_idx[room] ? s_room_temp_idx[room] - 1 : 1;
    if (left && idx > 0) {
        idx--;
    } else if (!left && idx < (int)TEMP_PRESET_COUNT - 1) {
        idx++;
    }
    s_room_temp_idx[room] = idx + 1;
    ESP_LOGI(TAG, "Remote arrow %s in room %d -> %d K", left ? "left" : "right", room, s_temp_presets[idx]);
    room_temp_send(room, s_temp_presets[idx]);
    room_pending_set(room, -1, -1, 1000000 / s_temp_presets[idx]);
    return true;
}

/* ---------- room view ----------
 * Lamps report one by one and also send in-between values while fading, so a
 * room's raw average jumps around. The page therefore sees:
 *  - after a command from this board: the commanded value until every lamp of
 *    the room has reached it (or 10 s have passed);
 *  - otherwise: the average, refreshed only once the room's reports have been
 *    quiet for 1.5 s (remote presses settle first). */

static void room_pending_set(int room, int on, int level, int mireds)
{
    for (int r = (room ? room : 1); r <= (room ? room : MAX_ROOM); r++) {
        room_pending_t *p = &s_pending[r];
        if (p->until_us == 0) {
            p->on = -1;
            p->level = -1;
            p->mireds = -1;
        }
        if (on >= 0) {
            p->on = on;
        }
        if (level >= 0) {
            p->level = level;
        }
        if (mireds >= 0) {
            p->mireds = mireds;
        }
        p->until_us = esp_timer_get_time() + PENDING_US;
    }
}

static bool room_reached_target(int r, const room_pending_t *p)
{
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].type != DEV_LAMP || s_devices[i].room != r) {
            continue;
        }
        const lamp_state_t *st = &s_state[i];
        if (p->on >= 0 && st->on != p->on) {
            return false;
        }
        if (p->level >= 0 && st->on == 1 && abs(st->level - p->level) > 3) {
            return false;
        }
        if (p->mireds >= 0 && st->mireds && abs(st->mireds - p->mireds) > 5) {
            return false;   /* lamps without colour temperature (mireds 0) are skipped */
        }
    }
    return true;
}

static room_view_t room_actual(int r)
{
    int known = 0, on = 0, lvl_n = 0, lvl_sum = 0, ct_n = 0, ct_sum = 0;
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].type != DEV_LAMP || s_devices[i].room != r) {
            continue;
        }
        const lamp_state_t *st = &s_state[i];
        if (st->on >= 0) {
            known++;
            on += st->on;
        }
        if (st->level && st->on != 0) {
            lvl_n++;
            lvl_sum += st->level;
        }
        if (st->mireds) {
            ct_n++;
            ct_sum += st->mireds;
        }
    }
    room_view_t v = { .on = -1, .level_pct = -1, .kelvin = -1 };
    if (known) {
        v.on = on > 0;
    }
    if (lvl_n) {
        v.level_pct = (int8_t)((lvl_sum / lvl_n * 100 + 127) / 254);
    }
    if (ct_n) {
        v.kelvin = (int16_t)(1000000 / (ct_sum / ct_n));
    }
    return v;
}

static void room_view_update(int r, int64_t now)
{
    room_pending_t *p = &s_pending[r];
    if (p->until_us && (now > p->until_us || room_reached_target(r, p))) {
        p->until_us = 0;
        s_room_last_report[r] = 0;          /* settled: take the real values right away */
    }
    if (!p->until_us && now - s_room_last_report[r] >= SETTLE_US) {
        s_room_view[r] = room_actual(r);
    } else if (s_room_view[r].on < 0 && s_room_view[r].level_pct < 0) {
        s_room_view[r] = room_actual(r);     /* nothing shown yet */
    }
    if (p->until_us) {                       /* show the commanded target meanwhile */
        if (p->on >= 0) {
            s_room_view[r].on = p->on;
        }
        if (p->level >= 0) {
            s_room_view[r].level_pct = (int8_t)((p->level * 100 + 127) / 254);
        }
        if (p->mireds > 0) {
            s_room_view[r].kelvin = (int16_t)(1000000 / p->mireds);
        }
    }
}

static void remember_room_temp(int room, int kelvin);

/* ---------- live lamp state ----------
 * Each lamp is bound to report On/Off, Level and Colour temperature to this
 * board, so the web page also shows changes made with a remote. */

static int device_by_short(uint16_t short_addr)
{
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].short_addr == short_addr) {
            return i;
        }
    }
    return -1;
}

static void state_update(uint16_t short_addr, uint16_t cluster, const esp_zb_zcl_attribute_t *attr)
{
    int idx = device_by_short(short_addr);
    if (idx < 0 || attr->data.value == NULL) {
        return;
    }
    lamp_state_t *st = &s_state[idx];
    lamp_state_t old = *st;
    if (cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && attr->id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        st->on = *(uint8_t *)attr->data.value ? 1 : 0;
    } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL && attr->id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
        st->level = *(uint8_t *)attr->data.value;
    } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL && attr->id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID) {
        st->mireds = *(uint16_t *)attr->data.value;
    } else {
        return;
    }
    ESP_LOGI(TAG, "Lamp #%d state: on %d level %d mireds %d", idx, st->on, st->level, st->mireds);
    /* Night mode: someone turned the light on or made it brighter -> start the 5 min wait again. */
    uint8_t room = s_devices[idx].room;
    if (room && room <= MAX_ROOM) {
        s_room_last_report[room] = esp_timer_get_time();
    }
    if (room && room <= MAX_ROOM &&
        ((st->on == 1 && old.on != 1) || (st->on == 1 && st->level > old.level + 2))) {
        s_bright_since[room] = esp_timer_get_time();
    }
}

static void lamp_config_report(const device_t *d, uint16_t cluster, uint16_t attr_id, uint8_t type, void *change)
{
    esp_zb_zcl_config_report_record_t rec = {
        .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID = attr_id,
        .attrType = type,
        .min_interval = 1,
        .max_interval = 600,
        .reportable_change = change,
    };
    esp_zb_zcl_config_report_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.src_endpoint = APP_ENDPOINT;
    cmd.zcl_basic_cmd.dst_endpoint = IKEA_ENDPOINT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = d->short_addr;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID = cluster;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.record_number = 1;
    cmd.record_field = &rec;
    esp_zb_zcl_config_report_cmd_req(&cmd);
}

static void lamp_read_attr(const device_t *d, uint16_t cluster, uint16_t attr_id)
{
    uint16_t attrs[1] = { attr_id };
    esp_zb_zcl_read_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.src_endpoint = APP_ENDPOINT;
    cmd.zcl_basic_cmd.dst_endpoint = IKEA_ENDPOINT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = d->short_addr;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID = cluster;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.attr_number = 1;
    cmd.attr_field = attrs;
    esp_zb_zcl_read_attr_cmd_req(&cmd);
}

static void lamp_setup_reporting(int idx)
{
    static uint8_t level_change = 1;
    static uint16_t mireds_change = 1;
    const device_t *d = &s_devices[idx];
    if (d->type != DEV_LAMP) {
        return;
    }
    esp_zb_ieee_addr_t me;
    esp_zb_get_long_address(me);
    static const uint16_t clusters[] = { ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                                         ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL };
    for (size_t i = 0; i < sizeof(clusters) / sizeof(clusters[0]); i++) {
        esp_zb_zdo_bind_req_param_t req = {0};
        memcpy(req.src_address, d->ieee, sizeof(esp_zb_ieee_addr_t));
        req.src_endp = IKEA_ENDPOINT;
        req.cluster_id = clusters[i];
        req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
        memcpy(req.dst_address_u.addr_long, me, sizeof(esp_zb_ieee_addr_t));
        req.dst_endp = APP_ENDPOINT;
        req.req_dst_addr = d->short_addr;
        esp_zb_zdo_device_bind_req(&req, NULL, NULL);
    }
    lamp_config_report(d, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL, NULL);
    lamp_config_report(d, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U8, &level_change);
    lamp_config_report(d, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U16, &mireds_change);
    lamp_read_attr(d, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    lamp_read_attr(d, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL, ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
    lamp_read_attr(d, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
    s_report_set[idx] = true;
}

/* At start-up: set up the lamps one by one, 1.5 s apart, to keep the air quiet. */
static void reporting_chain_cb(uint8_t from)
{
    for (int i = from; i < s_device_count; i++) {
        if (s_devices[i].type == DEV_LAMP) {
            lamp_setup_reporting(i);
            if (i + 1 < s_device_count) {
                esp_zb_scheduler_alarm((esp_zb_callback_t)reporting_chain_cb, (uint8_t)(i + 1), 1500);
            }
            return;
        }
    }
}

/* ---------- night mode ----------
 * Inside a room's night window, a light that stays on and bright for the room's
 * delay (5 minutes by default) is dimmed to the room's night percent (1 % by
 * default). Turning it on again or brightening restarts the wait.
 * Lights left on when the window starts also get 5 minutes, then dim. */

static bool night_in_window(const night_cfg_t *c, int hour)
{
    if (!c->enabled || c->from == c->to) {
        return false;
    }
    return c->from < c->to ? (hour >= c->from && hour < c->to) : (hour >= c->from || hour < c->to);
}

static int night_level(int r)
{
    int level = (s_night_pct[r] * 254 + 50) / 100;   /* 1 % -> 3 */
    return level < 1 ? 1 : level;
}

static void night_tick_cb(uint8_t unused)
{
    if (clock_valid()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        int64_t t = esp_timer_get_time();
        for (int r = 1; r <= MAX_ROOM; r++) {
            bool inw = night_in_window(&s_night[r], tm.tm_hour);
            if (inw && !s_in_window[r]) {
                s_bright_since[r] = t;          /* window just started: lights get 5 minutes */
            }
            s_in_window[r] = inw;
            if (!inw) {
                continue;
            }
            bool on = false;
            int level = 0;
            for (int i = 0; i < s_device_count; i++) {
                if (s_devices[i].type == DEV_LAMP && s_devices[i].room == r && s_state[i].on == 1) {
                    on = true;
                    level = s_state[i].level > level ? s_state[i].level : level;
                }
            }
            int target = night_level(r);
            if (!on || level <= target + NIGHT_MARGIN) {
                continue;
            }
            if (s_bright_since[r] == 0) {
                s_bright_since[r] = t;
            }
            if (t - s_bright_since[r] >= (int64_t)s_night_delay[r] * 60 * 1000000) {
                esp_zb_zcl_move_to_level_cmd_t cmd = {0};
                cmd.zcl_basic_cmd.src_endpoint = APP_ENDPOINT;
                cmd.zcl_basic_cmd.dst_addr_u.addr_short = (uint16_t)r;
                cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_GROUP_ENDP_NOT_PRESENT;
                cmd.level = target;
                cmd.transition_time = 30;       /* 3 s fade */
                esp_zb_zcl_level_move_to_level_cmd_req(&cmd);   /* without On/Off: never turns lights on */
                room_pending_set(r, -1, target, -1);
                char label[ROOM_NAME_LEN + 8];
                event_log("Gece modu: %s %%%d seviyesine kisildi", room_label(r, label, sizeof(label)), s_night_pct[r]);
                s_bright_since[r] = t;
            }
        }
    }
    esp_zb_scheduler_alarm((esp_zb_callback_t)night_tick_cb, 0, NIGHT_TICK_MS);
}

/* Replies from lamps: shows whether a command actually reached them. */
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t id, const void *message)
{
    switch (id) {
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID: {
        const esp_zb_zcl_cmd_default_resp_message_t *m = message;
        if (m->status_code != 0) {
            event_log("Cihaz 0x%04hx komutu reddetti: kume 0x%04x komut 0x%02x durum 0x%02x",
                      m->info.src_address.u.short_addr, m->info.cluster, m->resp_to_cmd, m->status_code);
        } else {
            ESP_LOGI(TAG, "Default resp OK from 0x%04hx cluster 0x%04x cmd 0x%02x",
                     m->info.src_address.u.short_addr, m->info.cluster, m->resp_to_cmd);
        }
        break;
    }
    case ESP_ZB_CORE_CMD_OPERATE_GROUP_RESP_CB_ID: {
        const esp_zb_zcl_groups_operate_group_resp_message_t *m = message;
        uint8_t st = m->info.status;
        if (st == 0x00 || st == 0x8a) {   /* added / already a member */
            event_log("Lamba 0x%04hx odanin grubunda (grup %d)", m->info.src_address.u.short_addr, m->group_id);
            int li = device_by_short(m->info.src_address.u.short_addr);
            if (li >= 0 && !s_report_set[li]) {
                lamp_setup_reporting(li);
            }
        } else {
            event_log("Lamba 0x%04hx gruba eklenemedi: durum 0x%02x", m->info.src_address.u.short_addr, st);
        }
        break;
    }
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
        const esp_zb_zcl_report_attr_message_t *m = message;
        state_update(m->src_address.u.short_addr, m->cluster, &m->attribute);
        break;
    }
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *m = message;
        for (esp_zb_zcl_read_attr_resp_variable_t *v = m->variables; v; v = v->next) {
            if (v->status == ESP_ZB_ZCL_STATUS_SUCCESS) {
                state_update(m->info.src_address.u.short_addr, m->info.cluster, &v->attribute);
            }
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "ZCL action 0x%x", id);
        break;
    }
    return ESP_OK;
}

static void esp_zb_task(void *pv)
{
    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,   /* IKEA devices do not use install codes */
        .nwk_cfg.zczr_cfg = {
            .max_children = MAX_CHILDREN,
        },
    };
    esp_zb_init(&zb_cfg);
    esp_zb_device_register(create_endpoint());
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_aps_data_indication_handler_register(aps_indication_handler);
    esp_zb_set_primary_network_channel_set(ZB_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void zb_start(void)
{
    storage_load();
    esp_zb_platform_config_t platform_cfg = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));
    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);
}

bool zb_is_pairing(void)
{
    return s_network_open;
}

bool zb_is_ready(void)
{
    return s_zb_ready;
}

/* ---------- public commands (any task) ---------- */

static esp_err_t fill_target(int room, esp_zb_zcl_basic_cmd_t *b, esp_zb_zcl_address_mode_t *mode)
{
    if (!s_zb_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    b->src_endpoint = APP_ENDPOINT;
    if (room == 0) {
        b->dst_addr_u.addr_short = 0xFFFD;   /* all rx-on-when-idle devices (lamps) */
        b->dst_endpoint = 0xFF;
        *mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        return ESP_OK;
    }
    if (room < 1 || room > MAX_ROOM) {
        return ESP_ERR_INVALID_ARG;
    }
    b->dst_addr_u.addr_short = (uint16_t)room;
    b->dst_endpoint = 0;
    *mode = ESP_ZB_APS_ADDR_MODE_16_GROUP_ENDP_NOT_PRESENT;
    return ESP_OK;
}

esp_err_t zb_onoff(int room, uint8_t zcl_cmd_id)
{
    esp_zb_zcl_on_off_cmd_t cmd = {0};
    ESP_RETURN_ON_ERROR(fill_target(room, &cmd.zcl_basic_cmd, &cmd.address_mode), TAG, "target");
    cmd.on_off_cmd_id = zcl_cmd_id;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_on_off_cmd_req(&cmd);
    if (zcl_cmd_id != ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID) {
        room_pending_set(room, zcl_cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_ON_ID ? 1 : 0, -1, -1);
    }
    esp_zb_lock_release();
    return ESP_OK;
}

/* Unicast to one lamp (diagnostics: the lamp answers with a Default Response). */
esp_err_t zb_onoff_device(int index, uint8_t zcl_cmd_id)
{
    if (!s_zb_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0 || index >= s_device_count) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_zb_zcl_on_off_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.src_endpoint = APP_ENDPOINT;
    cmd.zcl_basic_cmd.dst_endpoint = IKEA_ENDPOINT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = s_devices[index].short_addr;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.on_off_cmd_id = zcl_cmd_id;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
    return ESP_OK;
}

esp_err_t zb_level(int room, int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_zb_zcl_move_to_level_cmd_t cmd = {0};
    ESP_RETURN_ON_ERROR(fill_target(room, &cmd.zcl_basic_cmd, &cmd.address_mode), TAG, "target");
    cmd.level = percent == 0 ? 0 : (uint8_t)(1 + (percent * 253) / 100);
    cmd.transition_time = TRANSITION_DS;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_level_move_to_level_with_onoff_cmd_req(&cmd);
    if (percent == 0) {
        room_pending_set(room, 0, -1, -1);
    } else {
        room_pending_set(room, 1, cmd.level, -1);
    }
    esp_zb_lock_release();
    return ESP_OK;
}

esp_err_t zb_temp(int room, int kelvin)
{
    if (kelvin < 1000 || kelvin > 10000) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {0};
    ESP_RETURN_ON_ERROR(fill_target(room, &cmd.zcl_basic_cmd, &cmd.address_mode), TAG, "target");
    cmd.color_temperature = (uint16_t)(1000000 / kelvin);   /* mireds */
    cmd.transition_time = TRANSITION_DS;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
    room_pending_set(room, -1, -1, cmd.color_temperature);
    if (room == 0) {
        for (int r = 1; r <= MAX_ROOM; r++) {
            remember_room_temp(r, kelvin);
        }
    } else {
        remember_room_temp(room, kelvin);
    }
    esp_zb_lock_release();
    return ESP_OK;
}

esp_err_t zb_pair_start(int room, int seconds)
{
    if (!s_zb_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (room < 1 || room > MAX_ROOM || seconds < 1 || seconds > 254) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_zb_lock_acquire(portMAX_DELAY);
    s_pair_room = (uint8_t)room;
    esp_err_t err = esp_zb_bdb_open_network((uint8_t)seconds);
    esp_zb_lock_release();
    if (err == ESP_OK) {
        char label[ROOM_NAME_LEN + 8];
        event_log("Eslesme acik: %s, %d sn. Once lambalari, sonra kumandayi sifirla.",
                  room_label(room, label, sizeof(label)), seconds);
    }
    return err;
}

esp_err_t zb_pair_stop(void)
{
    if (!s_zb_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zb_bdb_open_network(0);
    esp_zb_lock_release();
    return err;
}

esp_err_t zb_assign(int index, int room)
{
    if (!s_zb_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (room < 1 || room > MAX_ROOM) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (index >= 0 && index < s_device_count) {
        device_assign_locked(index, (uint8_t)room);
        err = ESP_OK;
    }
    esp_zb_lock_release();
    return err;
}

esp_err_t zb_remote_arrows(int index)
{
    if (!s_zb_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (index >= 0 && index < s_device_count && s_devices[index].type == DEV_REMOTE) {
        rebind_start(index);
        event_log("Kumanda yeniden baglaniyor: 30 sn icinde kumandanin bir tusuna bas");
        err = ESP_OK;
    }
    esp_zb_lock_release();
    return err;
}

esp_err_t zb_forget(int index)
{
    if (s_zb_ready) {
        esp_zb_lock_acquire(portMAX_DELAY);
    }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (index >= 0 && index < s_device_count) {
        memmove(&s_devices[index], &s_devices[index + 1],
                sizeof(device_t) * (s_device_count - index - 1));
        memmove(&s_state[index], &s_state[index + 1], sizeof(lamp_state_t) * (s_device_count - index - 1));
        memmove(&s_report_set[index], &s_report_set[index + 1], sizeof(bool) * (s_device_count - index - 1));
        s_device_count--;
        devices_save();
        err = ESP_OK;
    }
    if (s_zb_ready) {
        esp_zb_lock_release();
    }
    if (err == ESP_OK) {
        event_log("Cihaz #%d listeden silindi", index);
    }
    return err;
}

esp_err_t zb_rename_room(int room, const char *name)
{
    if (room < 1 || room > MAX_ROOM || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_room_names[room], ROOM_NAME_LEN, "%s", name);
    rooms_save();
    return ESP_OK;
}

esp_err_t zb_set_room_icon(int room, int icon)
{
    if (room < 1 || room > MAX_ROOM || icon < 0 || icon > ROOM_ICON_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_room_icon[room] = (uint8_t)icon;
    rooms_save();
    return ESP_OK;
}

esp_err_t zb_set_night(int room, bool enabled, int from, int to, int pct, int delay_min)
{
    if (room < 1 || room > MAX_ROOM || from < 0 || from > 23 || to < 0 || to > 23 ||
        pct == 0 || pct < -1 || pct > 100 || delay_min == 0 || delay_min < -1 || delay_min > 120) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_zb_ready) {
        esp_zb_lock_acquire(portMAX_DELAY);
    }
    s_night[room] = (night_cfg_t){ .enabled = enabled, .from = (uint8_t)from, .to = (uint8_t)to };
    if (pct > 0) {
        s_night_pct[room] = (uint8_t)pct;
    }
    if (delay_min > 0) {
        s_night_delay[room] = (uint8_t)delay_min;
    }
    s_in_window[room] = false;      /* re-evaluated on the next tick */
    night_save();
    if (s_zb_ready) {
        esp_zb_lock_release();
    }
    return ESP_OK;
}

void zb_netreset(void)
{
    if (!s_zb_ready) {
        event_log("Zigbee calismiyor, ag sifirlanamadi");
        return;
    }
    s_device_count = 0;
    devices_save();
    memset(s_room_names, 0, sizeof(s_room_names));
    memset(s_room_icon, 0, sizeof(s_room_icon));
    rooms_save();
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_factory_reset();   /* erases zb_storage and restarts */
    esp_zb_lock_release();
}

void zb_get_snapshot(zb_snapshot_t *out)
{
    bool locked = s_zb_ready;
    if (locked) {
        esp_zb_lock_acquire(portMAX_DELAY);
    }
    out->ready = s_zb_ready;
    out->pairing = s_network_open;
    out->pair_room = s_pair_room;
    int64_t left = (s_pair_until_us - esp_timer_get_time()) / 1000000;
    out->pair_left = s_network_open && left > 0 ? (int)left : 0;
    out->pan_id = s_zb_ready ? esp_zb_get_pan_id() : 0;
    out->channel = s_zb_ready ? esp_zb_get_current_channel() : 0;
    out->device_count = s_device_count;
    memcpy(out->devices, s_devices, sizeof(device_t) * s_device_count);
    memcpy(out->states, s_state, sizeof(lamp_state_t) * s_device_count);
    memcpy(out->room_names, s_room_names, sizeof(s_room_names));
    memcpy(out->night, s_night, sizeof(s_night));
    memcpy(out->night_pct, s_night_pct, sizeof(s_night_pct));
    memcpy(out->night_delay, s_night_delay, sizeof(s_night_delay));
    memcpy(out->room_icons, s_room_icon, sizeof(s_room_icon));
    int64_t now = esp_timer_get_time();
    for (int r = 1; r <= MAX_ROOM; r++) {
        room_view_update(r, now);
    }
    memcpy(out->views, s_room_view, sizeof(s_room_view));
    if (locked) {
        esp_zb_lock_release();
    }
}
