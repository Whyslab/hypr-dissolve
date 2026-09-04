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
    # ВЫГРУЖАЕМ ВСЁ СВОЁ ДО загрузки новой копии, а не после.
    #
    # Две копии плагина в одной сессии — это два набора хуков на одни и те же
    # функции, и композитор падает со всеми открытыми окнами. Ровно так и
    # случилось однажды: старая сборка висела под именем .live.so, скрипт
    # выгружал только канонический путь, не находил её и добавлял вторую.
    for loaded in "$SO" "$ROOT"/build/hypr-dissolve.live*.so; do
        [ -e "$loaded" ] || continue
        hyprctl plugin unload "$loaded" >/dev/null 2>&1 || true
    done
    rm -f "$ROOT"/build/hypr-dissolve.live*.so

    STILL=$(hyprctl plugin list 2>/dev/null | grep -c "hypr-dissolve" || true)
    if [ "${STILL:-0}" -gt 0 ]; then
        echo "В сессии остался загруженный hypr-dissolve, которого нет среди известных путей." >&2
        echo "Загружать вторую копию нельзя — это уронит композитор. Выгрузите её вручную:" >&2
        echo "    hyprctl plugin list   # найдите путь" >&2
        echo "    hyprctl plugin unload <путь>" >&2
        echo "Либо перезайдите в сессию — плагин поднимется из конфига уже новым." >&2
        exit 1
    fi

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
