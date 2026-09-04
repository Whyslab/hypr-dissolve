#!/usr/bin/env bash
# Поднимает вложенный Hyprland с плагином и ждёт, пока он поднимется.
# Логи — в /tmp/.../nested.log, сигнатура экземпляра — в nested.sig.
set -u
OUT="${1:?укажите каталог для логов}"
mkdir -p "$OUT"
rm -f "$OUT/nested.sig"

# Вложенный Hyprland — это обычный wayland-клиент родительской сессии. Если
# WAYLAND_DISPLAY не задан (например, скрипт запущен из фонового задания или по
# ssh), он падает с «could not connect to wayland server», и это легко принять
# за поломку плагина. Подставляем первый живой сокет родителя.
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    for sock in "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/wayland-[0-9]*; do
        case "$sock" in *.lock) continue;; esac
        [ -S "$sock" ] || continue
        WAYLAND_DISPLAY=$(basename "$sock")
        export WAYLAND_DISPLAY
        echo "WAYLAND_DISPLAY не был задан, беру $WAYLAND_DISPLAY"
        break
    done
fi

export HYPRLAND_LOG_WLR=1
# Своя сигнатура: иначе hyprctl не отличит вложенный экземпляр от основного.
export HYPRLAND_INSTANCE_SIGNATURE_OVERRIDE=dissolve-test

Hyprland -c "$HOME/Projects/hypr-dissolve/test/nested.conf" > "$OUT/nested.log" 2>&1 &
echo $! > "$OUT/nested.pid"

# Ждём появления нового экземпляра. Ищем строго по pid запущенного процесса:
# сравнивать список экземпляров «с исходным» нельзя — стоит переменной с
# исходной сигнатурой оказаться пустой, и скрипт объявит своим экземпляром
# ЖИВУЮ сессию со всеми окнами пользователя.
NPID=$(cat "$OUT/nested.pid")
for i in $(seq 1 60); do
    sig=$(hyprctl instances -j 2>/dev/null | NPID="$NPID" python3 -c "
import sys, json, os
want = int(os.environ['NPID'])
try: d = json.load(sys.stdin)
except Exception: sys.exit(1)
for x in d:
    if x.get('pid') == want:
        print(x['instance']); break
" 2>/dev/null)
    if [ -n "$sig" ]; then break; fi
    sleep 0.5
done

if [ -z "${sig:-}" ]; then
    echo "TIMEOUT"
    exit 1
fi

echo "$sig" > "$OUT/nested.sig"

# Вложенный Hyprland не всегда создаёт окно вывода в родительской сессии: под
# логиндом, который уже держит сеат, DRM-бэкенд отваливается, а wayland-бэкенд
# в неинтерактивном окружении застревает после согласования dmabuf и до создания
# вывода. Без вывода нет рендера, а значит и нечего снимать. Виртуальный вывод
# решает это полностью: рендер идёт, grim его снимает.
for i in $(seq 1 20); do
    n=$(HYPRLAND_INSTANCE_SIGNATURE="$sig" hyprctl -j monitors 2>/dev/null | grep -c '"name"')
    [ "${n:-0}" -gt 0 ] && break
    if [ "$i" = 5 ]; then
        echo "вывода нет, создаю виртуальный"
        HYPRLAND_INSTANCE_SIGNATURE="$sig" hyprctl output create headless >/dev/null 2>&1
    fi
    sleep 0.5
done

echo "READY $sig"
exit 0
