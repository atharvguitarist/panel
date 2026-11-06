#!/usr/bin/env python3
"""Writes the build report the Arduino IDE prints after a compile -- flash
and static-RAM use with percentages -- as Markdown for the GitHub run summary,
plus the change against the previously published release.

  build_report.py <size -A output> <app .bin> <previous manifest.json> <versions.env>

Same section sets the Arduino ESP32 core uses (platform.txt recipe.size.regex)."""
import json, re, sys

FLASH_SECTIONS = {".iram0.text", ".iram0.vectors", ".dram0.data", ".flash.text",
                  ".flash.rodata", ".flash.appdesc", ".flash.init_array", ".eh_frame"}
RAM_SECTIONS   = {".dram0.data", ".dram0.bss", ".noinit"}
SLOT      = 1966080   # app partition, "Minimal SPIFFS" scheme
RAM_MAX   = 327680    # what the IDE reports as dynamic memory on the S3

def main():
    size_txt, binpath, prevpath, envpath = sys.argv[1:5]
    sec = {}
    for line in open(size_txt):
        m = re.match(r"^(\S+)\s+(\d+)\s+\d+", line)
        if m: sec[m.group(1)] = int(m.group(2))
    flash = sum(v for k, v in sec.items() if k in FLASH_SECTIONS)
    ram   = sum(v for k, v in sec.items() if k in RAM_SECTIONS)
    data  = open(binpath, "rb").read()
    tags  = set(re.findall(rb"PANELFW:(\d+\.\d+\.\d+)", data))
    ver   = tags.pop().decode() if len(tags) == 1 else "?"
    try:    prev = json.load(open(prevpath))
    except Exception: prev = {}
    pv, ps = prev.get("version", "-"), prev.get("size")
    delta  = f"{len(data) - ps:+,} bytes vs {pv}" if ps else "first release"
    env = dict(l.strip().split("=", 1) for l in open(envpath) if "=" in l and not l.startswith("#"))

    pct = lambda n, d: f"{100.0 * n / d:.1f}%"
    print(f"## Panel firmware v{ver}\n")
    print("| | Used | Of | |")
    print("|---|---:|---:|---|")
    print(f"| **App image** (`latest.bin`) | {len(data):,} B | {SLOT:,} B | {pct(len(data), SLOT)} of the slot, {SLOT - len(data):,} B free |")
    print(f"| **Program storage** (IDE figure) | {flash:,} B | {SLOT:,} B | {pct(flash, SLOT)} |")
    print(f"| **Static RAM** (data + bss) | {ram:,} B | {RAM_MAX:,} B | {pct(ram, RAM_MAX)}, {RAM_MAX - ram:,} B left for the heap |")
    print(f"| **Change** | {delta} | | |")
    print()
    print("| Section | Bytes |")
    print("|---|---:|")
    for k in (".flash.text", ".flash.rodata", ".iram0.text", ".dram0.data", ".dram0.bss"):
        if k in sec: print(f"| `{k}` | {sec[k]:,} |")
    print()
    print("Toolchain: ESP32 core " + env.get("ESP32_CORE", "?") + " · LVGL " + env.get("LVGL_VER", "?") +
          " · LovyanGFX " + env.get("LOVYAN_VER", "?") + " · ArduinoJson " + env.get("ARDUINOJSON_VER", "?") +
          " · PSRAM 8 MB (heap figures above are internal RAM only)")
    if len(data) > SLOT * 0.9:
        print(f"\n> **Warning:** the image is above 90% of the slot. Room for growth is {SLOT - len(data):,} bytes.")

if __name__ == "__main__":
    main()
