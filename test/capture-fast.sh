#!/usr/bin/env bash
set -u
SIG="$1"; OUT="$2"; SOCK="$3"; N="${4:-18}"; DELAY="${5:-0.07}"
export HYPRLAND_INSTANCE_SIGNATURE="$SIG"
mkdir -p "$OUT"; rm -f "$OUT"/f_*.png
WAYLAND_DISPLAY="$SOCK" grim "$OUT/f_000.png"
hyprctl dispatch closewindow class:kitty >/dev/null 2>&1
for i in $(seq 1 "$N"); do
    WAYLAND_DISPLAY="$SOCK" grim "$OUT/f_$(printf %03d $i).png" 2>/dev/null
    sleep "$DELAY"
done
echo "снято $(ls "$OUT"/f_*.png | wc -l)"
