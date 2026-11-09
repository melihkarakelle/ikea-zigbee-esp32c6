#!/bin/zsh
# Opens the serial console of the XIAO ESP32-C6 (exit with Ctrl+])
cd "$(dirname "$0")"
. ~/esp/esp-idf/export.sh >/dev/null 2>&1
exec idf.py -p "${1:-/dev/cu.usbmodem21101}" monitor
