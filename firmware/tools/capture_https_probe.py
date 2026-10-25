"""Capture one five-minute HTTPS probe window from the Waveshare serial port."""
from pathlib import Path
import serial
import sys
import time

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 300
log_path = Path(__file__).resolve().parents[1] / "build" / "https_probe_serial.log"
log_path.parent.mkdir(parents=True, exist_ok=True)

with serial.Serial(port, 115200, timeout=0.25) as serial_port, log_path.open("ab") as log:
    print(f"CAPTURE_READY port={port} log={log_path}", flush=True)
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = serial_port.readline()
        if line:
            log.write(line)
            log.flush()
print("CAPTURE_DONE", flush=True)
