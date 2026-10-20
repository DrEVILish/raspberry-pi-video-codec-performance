#!/bin/sh
# install-firmware-config.sh: hold the Raspberry Pi 4's video codec clocks at their stock maximum (and optionally
# overclock), by adding a marked block to /boot/firmware/config.txt.
#   sudo ./install-firmware-config.sh               codec-clocks.txt: H.264, ISP and core clock floors at stock max
#   sudo ./install-firmware-config.sh --overclock   the above plus overclock.txt (650 MHz codec/core/V3D, 2.0 GHz ARM)
#   sudo ./install-firmware-config.sh --remove      take the block out again
# The block sits at the end of config.txt between markers under [all], so running again replaces it. The previous
# file is kept as config.txt.codec-perf-bak. Nothing changes until you reboot; this script never reboots.
# Raspberry Pi 4 only (the Pi 5 has no H.264 decoder block and other clock names).
# Override the file with FIRMWARE_CONFIG=/path/to/config.txt (older images: /boot/config.txt).
set -e
dir="$(cd "$(dirname "$0")" && pwd)"
cfg="${FIRMWARE_CONFIG:-/boot/firmware/config.txt}"
[ -f "$cfg" ] || [ -n "${FIRMWARE_CONFIG:-}" ] || cfg=/boot/config.txt
begin="# >>> raspberry-pi-video-codec-performance (hardware/firmware-clocks) >>>"
end="# <<< raspberry-pi-video-codec-performance <<<"
mode=clocks
case "${1:-}" in
  "") ;;
  --overclock) mode=overclock ;;
  --remove) mode=remove ;;
  *) echo "usage: $0 [--overclock | --remove]" >&2; exit 2 ;;
esac
if [ -z "${FIRMWARE_CONFIG:-}" ]; then
  model="$(tr -d '\0' 2>/dev/null < /proc/device-tree/model || true)"
  case "$model" in
    "Raspberry Pi 4"*|"Raspberry Pi Compute Module 4"*|"Raspberry Pi 400"*) ;;
    *) echo "not a Raspberry Pi 4 (${model:-unknown}): nothing changed" >&2; exit 1 ;;
  esac
fi
[ -f "$cfg" ] || { echo "$cfg not found" >&2; exit 1; }
cp -p "$cfg" "$cfg.codec-perf-bak"
tmp="$(mktemp)"
awk -v b="$begin" -v e="$end" '$0 == b { skip = 1; next } $0 == e { skip = 0; next } !skip' "$cfg" > "$tmp"
if [ "$mode" != remove ]; then
  {
    echo "$begin"
    echo "[all]"
    cat "$dir/codec-clocks.txt"
    [ "$mode" = overclock ] && cat "$dir/overclock.txt"
    echo "$end"
  } >> "$tmp"
fi
cat "$tmp" > "$cfg"
rm -f "$tmp"
echo "$mode settings $( [ "$mode" = remove ] && echo removed from || echo written to ) $cfg (backup: $cfg.codec-perf-bak). Reboot to apply."
