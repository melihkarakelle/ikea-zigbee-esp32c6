/* USB serial console (backup to the web page). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_zigbee_core.h"
#include "app.h"

static int report(esp_err_t err)
{
    if (err == ESP_OK) {
        return 0;
    }
    printf("Hata: %s\n", err == ESP_ERR_INVALID_STATE ? "Zigbee agi hazir degil" :
                         err == ESP_ERR_INVALID_ARG   ? "gecersiz deger" : esp_err_to_name(err));
    return 1;
}

/* "all" -> 0, otherwise the room number. */
static int parse_room(const char *arg)
{
    return strcmp(arg, "all") == 0 ? 0 : atoi(arg);
}

static int onoff(int argc, char **argv, uint8_t id)
{
    if (argc < 2) {
        printf("Kullanim: %s <oda|all>\n", argv[0]);
        return 1;
    }
    return report(zb_onoff(parse_room(argv[1]), id));
}

static int cmd_on(int argc, char **argv)     { return onoff(argc, argv, ESP_ZB_ZCL_CMD_ON_OFF_ON_ID); }
static int cmd_off(int argc, char **argv)    { return onoff(argc, argv, ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID); }
static int cmd_toggle(int argc, char **argv) { return onoff(argc, argv, ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID); }

static int cmd_dim(int argc, char **argv)
{
    if (argc < 3) {
        printf("Kullanim: dim <oda|all> <0-100>\n");
        return 1;
    }
    return report(zb_level(parse_room(argv[1]), atoi(argv[2])));
}

static int cmd_temp(int argc, char **argv)
{
    if (argc < 3) {
        printf("Kullanim: temp <oda|all> <warm|neutral|cold|Kelvin>\n");
        return 1;
    }
    int k = !strcmp(argv[2], "warm") ? 2200 : !strcmp(argv[2], "neutral") ? 2700 :
            !strcmp(argv[2], "cold") ? 4000 : atoi(argv[2]);
    return report(zb_temp(parse_room(argv[1]), k));
}

static int cmd_pair(int argc, char **argv)
{
    if (argc < 2) {
        printf("Kullanim: pair <oda> [saniye] | pair stop\n");
        return 1;
    }
    if (!strcmp(argv[1], "stop")) {
        return report(zb_pair_stop());
    }
    return report(zb_pair_start(atoi(argv[1]), argc >= 3 ? atoi(argv[2]) : 180));
}

static int cmd_list(int argc, char **argv)
{
    static zb_snapshot_t s;
    zb_get_snapshot(&s);
    printf(" #  tip      oda  short   ieee\n");
    for (int i = 0; i < s.device_count; i++) {
        char ieee[17];
        zb_ieee_to_str(s.devices[i].ieee, ieee, sizeof(ieee));
        printf("%2d  %-7s  %3d  0x%04x  %s\n", i, zb_type_name(s.devices[i].type),
               s.devices[i].room, s.devices[i].short_addr, ieee);
    }
    printf("Toplam %d cihaz. Eslesme %s. Zigbee kanal %d.\n", s.device_count,
           s.pairing ? "acik" : "kapali", s.channel);
    return 0;
}

static int cmd_assign(int argc, char **argv)
{
    if (argc < 3) {
        printf("Kullanim: assign <#> <oda>\n");
        return 1;
    }
    return report(zb_assign(atoi(argv[1]), atoi(argv[2])));
}

static int cmd_forget(int argc, char **argv)
{
    if (argc < 2) {
        printf("Kullanim: forget <#>\n");
        return 1;
    }
    return report(zb_forget(atoi(argv[1])));
}

static int cmd_ip(int argc, char **argv)
{
    printf("%s\n", wifi_status_text());
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("Kullanim: wifi <ssid> [sifre]   |   wifi reset\n");
        return 1;
    }
    if (!strcmp(argv[1], "reset")) {
        wifi_clear_credentials();
    } else if (report(wifi_save_credentials(argv[1], argc >= 3 ? argv[2] : ""))) {
        return 1;
    }
    printf("Kaydedildi, yeniden baslatiliyor...\n");
    esp_restart();
    return 0;
}

static int cmd_netreset(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "EVET") != 0) {
        printf("Zigbee agini, cihaz listesini ve oda adlarini SILER. Tum lamba ve kumandalar\n"
               "yeniden eslenmek zorunda kalir. Emin isen: netreset EVET\n");
        return 1;
    }
    zb_netreset();
    return 0;
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "ikea>";
    esp_console_dev_usb_serial_jtag_config_t hw_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command = "pair",     .help = "Odaya cihaz eslemeyi ac: pair <oda> [saniye] | pair stop", .func = cmd_pair },
        { .command = "on",       .help = "on <oda|all>",                           .func = cmd_on },
        { .command = "off",      .help = "off <oda|all>",                          .func = cmd_off },
        { .command = "toggle",   .help = "toggle <oda|all>",                       .func = cmd_toggle },
        { .command = "dim",      .help = "dim <oda|all> <0-100>",                  .func = cmd_dim },
        { .command = "temp",     .help = "temp <oda|all> <warm|neutral|cold|Kelvin>", .func = cmd_temp },
        { .command = "list",     .help = "Cihaz listesi",                          .func = cmd_list },
        { .command = "assign",   .help = "assign <#> <oda>",                       .func = cmd_assign },
        { .command = "forget",   .help = "forget <#>",                             .func = cmd_forget },
        { .command = "ip",       .help = "WiFi durumu ve web adresi",              .func = cmd_ip },
        { .command = "wifi",     .help = "wifi <ssid> [sifre] | wifi reset",       .func = cmd_wifi },
        { .command = "netreset", .help = "Zigbee agini tamamen sil: netreset EVET", .func = cmd_netreset },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
