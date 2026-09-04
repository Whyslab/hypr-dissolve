#!/usr/bin/env bash
# Сборка и загрузка плагина. Конфиг пользователя намеренно не трогаем —
# строку plugin = ... добавляйте сами, осознанно.
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(pwd)"

HAVE=$(pkg-config --modversion hyprland 2>/dev/null || true)
if [ -z "$HAVE" ]; then
    echo "Заголовки Hyprland не найдены. На Arch они в пакете hyprland." >&2
    exit 1
fi
echo "Заголовки Hyprland: $HAVE"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)"

SO="$ROOT/build/hypr-dissolve.so"
[ -f "$SO" ] || { echo "Сборка не дала $SO" >&2; exit 1; }
echo "Собрано: $SO"

if [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]; then
    hyprctl plugin unload "$SO" >/dev/null 2>&1 || true

    # dlopen различает библиотеки по ПУТИ, а не по содержимому. Пока прежний
    # образ ещё висит в памяти композитора (dlclose откладывается, если на
    # библиотеку кто-то ссылается), повторный dlopen того же пути вернёт СТАРЫЙ
    # образ — и hyprctl честно ответит «ok», хотя в сессии продолжит работать
    # предыдущая сборка. Поэтому в текущую сессию грузим копию под уникальным
    # именем; строка plugin = в конфиге по-прежнему указывает на канонический
    # путь, и при следующем входе поднимется он.
    LIVE="$ROOT/build/hypr-dissolve.live-$(date +%s).so"
    cp -f "$SO" "$LIVE"

    if hyprctl plugin load "$LIVE" | grep -qi ok; then
        echo "Плагин загружен в текущую сессию."
        # Прежние копии больше не нужны: выгружаем и убираем.
        for old in "$ROOT"/build/hypr-dissolve.live*.so; do
            [ "$old" = "$LIVE" ] && continue
            [ -e "$old" ] || continue
            hyprctl plugin unload "$old" >/dev/null 2>&1 || true
            rm -f "$old"
        done
    else
        rm -f "$LIVE"
        echo "Загрузить не удалось — смотрите уведомление Hyprland." >&2
    fi
fi

cat <<TXT

Чтобы плагин поднимался при входе, добавьте в ~/.config/hypr/hyprland.conf:

    plugin = $SO

ПОМНИТЕ: после каждого обновления пакета hyprland запускайте этот скрипт
заново — плагин привязан к ABI конкретной сборки.
TXT
