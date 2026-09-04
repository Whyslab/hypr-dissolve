#!/usr/bin/env bash
# Закрывает окно во вложенном Hyprland и снимает серию кадров, чтобы увидеть
# анимацию распада покадрово: одиночный скриншот её не поймает.
set -u
SIG="${1:?сигнатура}"; OUT="${2:?каталог}"; SOCK="${3:?wayland-N}"
export HYPRLAND_INSTANCE_SIGNATURE="$SIG"
mkdir -p "$OUT"; rm -f "$OUT"/frame_*.png

grim -o WAYLAND-1 "$OUT/frame_00_before.png" 2>/dev/null || WAYLAND_DISPLAY="$SOCK" grim "$OUT/frame_00_before.png"

hyprctl dispatch closewindow class:kitty >/dev/null 2>&1

for i in $(seq 1 10); do
    WAYLAND_DISPLAY="$SOCK" grim "$OUT/frame_$(printf %02d $i).png" 2>/dev/null
    sleep 0.12
done
echo "снято: $(ls "$OUT"/frame_*.png 2>/dev/null | wc -l) кадров"
