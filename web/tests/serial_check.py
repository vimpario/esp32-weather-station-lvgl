#!/usr/bin/env python3
"""§18 serial check: capture the ESP32 boot + NORMAL logging and analyse it.

Usage: python serial_check.py [PORT] [SECONDS]

Resets the board through DTR/RTS so the capture contains a real boot banner, then
reports: Wi-Fi connected, static contract, HTTP server started, and the absence of
[ERROR]/reset/reconnect patterns.
"""
import re
import sys
import time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 45

with serial.Serial(PORT, 115200, timeout=0.2) as port:
    # esptool-style reset into normal run mode (GPIO0 high, EN pulse).
    port.setDTR(False)
    port.setRTS(True)
    time.sleep(0.15)
    port.setRTS(False)
    time.sleep(0.05)
    port.reset_input_buffer()
    print(f"serial: capturing {SECONDS}s from {PORT} after reset")
    deadline = time.time() + SECONDS
    lines = []
    while time.time() < deadline:
        raw = port.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", errors="replace").rstrip()
        if text:
            lines.append(text)
            print("  " + text)

text = "\n".join(lines)
print("\n== serial analysis ==")
checks = [
    ("boot banner present", bool(re.search(r"\[BOOT\]", text))),
    ("Wi-Fi connected", bool(re.search(r"Wi-?Fi.*(connected|CONNECTED)|connected to", text, re.I))),
    ("static IP asserted", "192.168.1.111" in text),
    ("HTTP server started", bool(re.search(r"HTTP|server.*(start|listen)|API", text, re.I))),
    ("no [ERROR] lines", "[ERROR]" not in text and "[E]" not in text),
    ("single power-on reset reason only", len(re.findall(r"reset_reason=", text)) <= 1),
    ("no repeated reconnect", len(re.findall(r"(?i)reconnect|disconnect", text)) == 0),
    ("sensor data reported", bool(re.search(r"BMP280|DHT11|temperature|humidity|pressure", text, re.I))),
    ("heap reported", bool(re.search(r"heap|free", text, re.I))),
]
ok = 0
for name, passed in checks:
    print(f"  [{'OK' if passed else 'FAIL'}] {name}")
    ok += 1 if passed else 0
print(f"serial checks: {ok}/{len(checks)} passed")
with open("serial_capture.log", "w", encoding="utf-8") as handle:
    handle.write(text)
print("log saved: serial_capture.log")
sys.exit(0 if ok == len(checks) else 1)
