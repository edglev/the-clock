#!/usr/bin/env python3
"""Usage: python screenshot_receiver.py <serial_port>

Press 's' in the monitor to trigger a screenshot, or use:
  echo s > <serial_port>
Then run this script to receive and save the image.
"""
import sys
import serial
from PIL import Image

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    ser = serial.Serial(port, 115200, timeout=30)
    ser.reset_input_buffer()

    ser.write(b"s")

    buf = b""
    while True:
        c = ser.read(1)
        if not c:
            print("timeout")
            return
        buf += c
        idx = buf.find(b"SS:")
        if idx >= 0:
            buf = buf[idx:]
            break

    header_end = buf.find(b"\n")
    if header_end < 0:
        more = ser.read_until(b"\n")
        buf += more
        header_end = buf.find(b"\n")

    header = buf[3:header_end].decode()
    parts = header.split(":")
    w, h = int(parts[0]), int(parts[1])
    print(f"Receiving {w}x{h} RGB565...")

    data_start = header_end + 1
    remaining = buf[data_start:]
    total = w * h * 2
    data = bytearray(remaining)
    while len(data) < total:
        chunk = ser.read(total - len(data))
        if not chunk:
            break
        data.extend(chunk)

    data = bytes(data[:total])
    img = Image.frombytes("RGB", (w, h), data, "raw", "BGR;16")
    img.save("screenshot.png")
    print(f"Saved screenshot.png ({w}x{h})")

if __name__ == "__main__":
    main()
