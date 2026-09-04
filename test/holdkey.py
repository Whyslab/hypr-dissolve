#!/usr/bin/env python3
"""Держит клавишу через /dev/uinput настоящим событием ядра.

hyprctl dispatch sendkeystate синтезирует событие мимо списка зажатых клавиш
композитора, поэтому для проверки утечки через wl_keyboard.enter он не годится:
массив приходит пустым независимо от того, есть баг или нет.

Использование:
    holdkey.py down            — нажать и держать (демон остаётся жив)
    holdkey.py tap <секунды>   — нажать, подождать, отпустить
"""
import ctypes
import fcntl
import os
import struct
import sys
import time

KEY_ESC = 1
KEY_SPACE = 57
EV_KEY, EV_SYN, SYN_REPORT = 0x01, 0x00, 0x00

UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502
UI_DEV_SETUP = 0x405C5503
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565


class UinputSetup(ctypes.Structure):
    _fields_ = [
        ("bustype", ctypes.c_uint16),
        ("vendor", ctypes.c_uint16),
        ("product", ctypes.c_uint16),
        ("version", ctypes.c_uint16),
        ("name", ctypes.c_char * 80),
        ("ff_effects_max", ctypes.c_uint32),
    ]


def emit(fd, etype, code, value):
    # struct input_event на x86_64: timeval(16) + type(2) + code(2) + value(4)
    os.write(fd, struct.pack("qqHHi", 0, 0, etype, code, value))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "tap"
    hold = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    key  = int(sys.argv[3]) if len(sys.argv) > 3 else KEY_ESC

    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    # Устройство обязано ЗАЯВИТЬ каждую клавишу, которую собирается слать:
    # незаявленный код ядро молча выбрасывает, и выглядит это как «нажатие не
    # дошло». Заявляем весь диапазон обычных клавиш.
    for code in range(1, 128):
        fcntl.ioctl(fd, UI_SET_KEYBIT, code)

    setup = UinputSetup(bustype=0x03, vendor=0x1234, product=0x5678, version=1,
                        name=b"dissolve-test-keyboard", ff_effects_max=0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1.5)  # даём композитору заметить новое устройство

    if mode != "uponly":
        emit(fd, EV_KEY, key, 1)
        emit(fd, EV_SYN, SYN_REPORT, 0)
        print(f"клавиша {key} нажата", flush=True)
        time.sleep(hold)

    emit(fd, EV_KEY, key, 0)
    emit(fd, EV_SYN, SYN_REPORT, 0)
    print(f"клавиша {key} отпущена", flush=True)

    time.sleep(0.2)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)


if __name__ == "__main__":
    main()
