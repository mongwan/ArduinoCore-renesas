#!/usr/bin/env python3
"""Time-limited serial capture with timestamps.

  python3 capture.py COM5 600 anchor.log          # Windows
  python3 capture.py /dev/cu.usbmodem1101 600 anchor.log   # macOS

Every line is written as "<seconds since start> <line>". When the port goes
away (watchdog reset, upload, cable), the capture notes it and reopens the
port as soon as it comes back, until the time is up. Needs pyserial
(pip install pyserial).
"""
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: pip install pyserial")


def open_port(port, until):
    while time.time() < until:
        try:
            ser = serial.Serial(port, 115200, timeout=0.5)
            ser.dtr = True
            return ser
        except serial.SerialException:
            time.sleep(0.5)
    return None


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    t0 = time.time()
    end = t0 + secs
    with open(out, "w", encoding="utf-8", errors="replace") as f:
        def write(text):
            f.write(f"{time.time() - t0:8.2f} {text}\n")
            f.flush()

        ser = open_port(port, end)
        if ser is None:
            sys.exit(f"could not open {port}")
        buf = b""
        while time.time() < end:
            try:
                buf += ser.read(4096)
            except (serial.SerialException, OSError) as e:
                write(f"[capture] port lost: {e}")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = open_port(port, end)
                if ser is None:
                    break
                write("[capture] port reopened")
                continue
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                write(line.decode("utf-8", "replace").rstrip())
        if buf:
            write(buf.decode("utf-8", "replace").rstrip())
        if ser is not None:
            ser.close()


if __name__ == "__main__":
    main()
