#!/usr/bin/env python3
"""通过 USB 串口向设备发送调试按键并打印日志。

用法：devkeys.py [-p 端口] 'e 3 e 3 d 2'   # 字母是按键（见 app/input.cc），数字是等待秒数
"""
import argparse
import sys
import threading
import time

import serial

p = argparse.ArgumentParser()
p.add_argument("-p", "--port", default="/dev/cu.usbmodem2101")
p.add_argument("script")
p.add_argument("--tail", type=float, default=3)
p.add_argument("--dump", help="把 p 键导出的帧依次保存为 PNG，路径前缀")
args = p.parse_args()

s = serial.Serial()
s.port = args.port
s.baudrate = 115200
s.dtr = False
s.rts = False
s.timeout = 0.1
s.open()

stop = False
frame = {}
saved = []


def save_png(path, rows):
    import struct
    import zlib
    raw = b"".join(b"\x00" + rows.get(y, b"\xff" * 50) for y in range(300))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 400, 300, 1, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)
KEEP = {"main", "app", "audio", "epd", "input", "library", "sleep", "state"}


def reopen():
    # 深睡时 USB 断开，等设备重新枚举后接着读
    global s
    print(f"{time.monotonic() - t0:7.2f} -- port lost, waiting", flush=True)
    while not stop:
        try:
            s.close()
            s.open()
            print(f"{time.monotonic() - t0:7.2f} -- port back", flush=True)
            return
        except (serial.SerialException, OSError):
            time.sleep(0.2)


def reader():
    buf = b""
    while not stop:
        try:
            buf += s.read(4096)
        except (serial.SerialException, OSError):
            reopen()
            continue
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip()
            # 去掉日志颜色码
            text = text.replace("\x1b[0;32m", "").replace("\x1b[0;33m", "").replace("\x1b[0;31m", "").replace("\x1b[0m", "")
            if text.startswith("FB:"):
                _, row, data = text.split(":", 2)
                frame[int(row)] = bytes.fromhex(data)
                if int(row) == 299 and args.dump:
                    save_png(f"{args.dump}{len(saved)}.png", frame)
                    saved.append(1)
                    print(f"{time.monotonic() - t0:7.2f} -- saved {args.dump}{len(saved) - 1}.png", flush=True)
                continue
            # 跳过 ROM / bootloader / 启动阶段的系统日志
            if not text or not text.startswith(("I (", "W (", "E (")) and "***" not in text and "Backtrace" not in text and "Guru" not in text:
                continue
            if text.startswith("I (") and text.split(")", 1)[1].split(":")[0].strip() not in KEEP:
                continue
            if text:
                print(f"{time.monotonic() - t0:7.2f} | {text}", flush=True)


t0 = time.monotonic()
th = threading.Thread(target=reader, daemon=True)
th.start()
for tok in args.script.split():
    try:
        time.sleep(float(tok))
    except ValueError:
        print(f"{time.monotonic() - t0:7.2f} >> {tok}", flush=True)
        try:
            s.write(tok.encode())
        except (serial.SerialException, OSError):
            print("   write failed (port down)", flush=True)
time.sleep(args.tail)
stop = True
th.join()
