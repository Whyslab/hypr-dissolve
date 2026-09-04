#!/usr/bin/env bash
# Поднимает вложенный Hyprland с плагином и ждёт, пока он поднимется.
# Логи — в /tmp/.../nested.log, сигнатура экземпляра — в nested.sig.
set -u
OUT="${1:?укажите каталог для логов}"
mkdir -p "$OUT"
rm -f "$OUT/nested.sig"

export HYPRLAND_LOG_WLR=1
# Своя сигнатура: иначе hyprctl не отличит вложенный экземпляр от основного.
export HYPRLAND_INSTANCE_SIGNATURE_OVERRIDE=dissolve-test

Hyprland -c "$HOME/Projects/hypr-dissolve/test/nested.conf" > "$OUT/nested.log" 2>&1 &
echo $! > "$OUT/nested.pid"

# Ждём появления нового сокета: сравниваем список экземпляров с исходным.
for i in $(seq 1 60); do
    sig=$(hyprctl instances -j 2>/dev/null | python3 -c "
import sys,json,os
try: d=json.load(sys.stdin)
except Exception: sys.exit(1)
cur=os.environ.get('MAIN_SIG','')
for x in d:
    if x['instance']!=cur: print(x['instance']); break
" 2>/dev/null)
    if [ -n "$sig" ]; then echo "$sig" > "$OUT/nested.sig"; echo "READY $sig"; exit 0; fi
    sleep 0.5
done
echo "TIMEOUT"
exit 1
