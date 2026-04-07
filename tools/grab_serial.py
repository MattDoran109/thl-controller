"""
Quick serial log grabber — opens COM11 at 115200 and streams output
for a fixed number of seconds. Retries until the port is free.
Usage: python tools/grab_serial.py [seconds] [port]
"""
import sys
import time
import serial

PORT    = sys.argv[2] if len(sys.argv) > 2 else "COM11"
SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 45

print(f"Waiting for {PORT} to become available...", flush=True)
ser = None
for attempt in range(60):
    try:
        ser = serial.Serial(PORT, 115200, timeout=0.1)
        print(f"Opened {PORT} after {attempt} retries", flush=True)
        break
    except serial.SerialException:
        time.sleep(0.5)

if ser is None:
    print(f"Could not open {PORT} after 30 s — giving up")
    sys.exit(1)

deadline = time.time() + SECONDS
print(f"Capturing for {SECONDS} s — Ctrl-C to stop early", flush=True)
print("-" * 60, flush=True)
try:
    while time.time() < deadline:
        line = ser.readline()
        if line:
            try:
                txt = line.decode("utf-8", errors="replace").rstrip()
                print(txt, flush=True)
            except Exception:
                pass
except KeyboardInterrupt:
    pass
finally:
    ser.close()
    print("-" * 60)
    print("Done")
