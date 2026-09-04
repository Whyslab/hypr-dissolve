#!/usr/bin/env bash
# Снимает закрытие СЛОЯ (rofi) во вложенном Hyprland и печатает профиль кадров.
#
# Зачем отдельно от capture.sh: слой закрывается не диспетчером композитора, а
# завершением процесса, и живёт на своей ветке анимаций (fadeLayersOut), а не на
# fadeOut. Перепутать эти два пути легко, а симптом один — «эффекта нет».
#
#   ./test/run-nested.sh /tmp/dissolve && ./test/capture-layer.sh /tmp/dissolve
set -u

OUT="${1:?укажите тот же каталог, что и run-nested.sh}"
FRAMES="${FRAMES:-120}"

SIG=$(cat "$OUT/nested.sig")
export HYPRLAND_INSTANCE_SIGNATURE="$SIG"

WL=$(hyprctl instances -j | SIG="$SIG" python3 -c "
import sys, json, os
want = os.environ['SIG']
print(next(x['wl_socket'] for x in json.load(sys.stdin) if x['instance'] == want))
")

MON=$(hyprctl -j monitors | python3 -c "import sys, json; print(json.load(sys.stdin)[0]['name'])")
echo "экземпляр $SIG на $WL, вывод $MON"

SHOTS="$OUT/layer-shots"
rm -rf "$SHOTS"
mkdir -p "$SHOTS"

# Тема ужимается под размер вывода: вложенное окно обычно меньше настоящего
# экрана, а rofi по умолчанию рисует список на 10 строк и не влезает.
hyprctl dispatch exec "rofi -show drun -theme-str 'window {width: 380px;} listview {lines: 4;}'" >/dev/null
sleep 2.5

( for i in $(seq -w 1 "$FRAMES"); do
      WAYLAND_DISPLAY="$WL" grim -o "$MON" "$SHOTS/f$i.png" 2>/dev/null
  done ) &
CAP=$!

sleep 0.3
# -x, а не -f: шаблон -f совпал бы с командной строкой самого скрипта, и он
# убил бы себя. Проверено на собственной шкуре.
pkill -x rofi
wait $CAP

python3 - "$SHOTS" <<'PY'
import os, sys, glob

fs = sorted(glob.glob(os.path.join(sys.argv[1], "*.png")))
sz = [os.path.getsize(f) for f in fs]
if not sz:
    print("кадров нет — вывод не снялся")
    raise SystemExit(1)

peak = max(sz)
print(f"кадров {len(sz)}, старт {sz[0]}, максимум {peak}, конец {sz[-1]}")
print(f"рост {peak / sz[0]:.2f}x")
print()
print("Целое окно жмётся хорошо, распад даёт шум и файл распухает. Рост около")
print("2x и больше — эффект идёт. Рост около 1.2x — это обычное затухание,")
print("распад не включился.")
print()
for f, s in list(zip(fs, sz))[::max(1, len(sz) // 30)]:
    print(f"{os.path.basename(f)} {s:8d} {'#' * int(46 * s / peak)}")
PY
