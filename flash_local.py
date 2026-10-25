"""Flash this build using its own metadata, with bounded chunk retries.
Run with the ESP-IDF virtual environment Python: flash_local.py [COM6].
Does not erase the full chip or write NVS. Saves diagnostic output locally.
"""
import json
from pathlib import Path
import subprocess
import sys
import time

BUILD = Path(__file__).resolve().parent / "firmware" / "build"
PORT = sys.argv[1] if len(sys.argv) > 1 else "COM6"
CHUNK = 128 * 1024


def invoke(args):
    result = subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", "esp32s3",
         "--port", PORT, "--baud", "460800"] + args,
        capture_output=True, text=True, errors="replace", timeout=90)
    with (BUILD / "local_flash.log").open("a", encoding="utf-8") as log:
        log.write(result.stdout + result.stderr)
    return result


def main():
    meta = json.loads((BUILD / "flasher_args.json").read_text())
    files = sorted((int(offset, 0), BUILD / name)
                   for offset, name in meta["flash_files"].items())
    # Validate every input before starting any write.
    for address, path in files:
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError("Missing or empty image: " + str(path))
        if address % 4096 or address + path.stat().st_size > 16 * 1024 * 1024:
            raise RuntimeError("Invalid flash range: " + str(path))
        print(hex(address), path.name, path.stat().st_size, flush=True)
    chunks = BUILD / "local_flash_chunks"
    chunks.mkdir(exist_ok=True)
    for address, path in files:
        data = path.read_bytes()
        for start in range(0, len(data), CHUNK):
            target = address + start
            piece = chunks / ("%08x.bin" % target)
            piece.write_bytes(data[start:start + CHUNK])
            for attempt in range(1, 4):
                try:
                    result = invoke(["--before", "default_reset", "--after", "no_reset",
                                     "write_flash"] + meta["write_flash_args"] +
                                    [hex(target), str(piece)])
                    if result.returncode == 0 and "Hash of data verified" in result.stdout:
                        print("Verified %s at %s (attempt %d)" %
                              (path.name, hex(target), attempt), flush=True)
                        break
                except subprocess.TimeoutExpired:
                    print("Timeout at " + hex(target), flush=True)
                if attempt == 3:
                    raise RuntimeError("Flash failed at " + hex(target) + "; inspect local_flash.log")
                time.sleep(1)
    # Verify complete images as well as individual chunks.
    for address, path in files:
        result = invoke(["--before", "default_reset", "--after", "no_reset",
                         "verify_flash", hex(address), str(path)])
        if result.returncode:
            raise RuntimeError("Full-image verification failed: " + str(path))
        print("Full-image verified: " + path.name, flush=True)
    result = invoke(["--before", "default_reset", "--after", "hard_reset", "chip_id"])
    if result.returncode:
        raise RuntimeError("Images verified, but reset failed; press RESET manually")
    print("All images verified; reset requested. Check boot log separately.", flush=True)


if __name__ == "__main__":
    main()
