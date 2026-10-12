#!/usr/bin/env python3
"""
Prepare a release for the panel's update-from-GitHub feature.

Run this from the repo folder (the one containing index.html).

FIRMWARE RELEASE  (after Sketch > Export Compiled Binary in Arduino IDE):

    python make_release.py "D:\\sketchbook\\Panel_v6\\build\\esp32.esp32.esp32s3\\Panel_v6.ino.bin" 5.6.4

    Writes firmware/latest.bin and firmware/manifest.json. Commit both.
    The version must match FW_VERSION in the sketch you built, and must be
    higher than what the panels are running, or they report "already up to
    date" and do nothing -- which is the safe behaviour.

FESTIVAL DATES ONLY  (after editing config/festivals.txt, no new firmware):

    python make_release.py --festivals

    Rewrites only the "festivals" stamp in firmware/manifest.json. Commit
    config/festivals.txt and firmware/manifest.json together. Panels fetch
    the new calendar at their next check (within a day) -- and only then.

Both modes stamp the manifest with the md5 prefix of config/festivals.txt
when that file exists. No file, no stamp, and panels keep their SD copy.
"""
import hashlib, json, os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW_DIR = os.path.join(HERE, "firmware")
MANIFEST = os.path.join(FW_DIR, "manifest.json")
FESTIVALS = os.path.join(HERE, "config", "festivals.txt")

def festival_stamp():
    if not os.path.isfile(FESTIVALS):
        return ""
    return hashlib.md5(open(FESTIVALS, "rb").read()).hexdigest()[:8]

def write_manifest(manifest):
    os.makedirs(FW_DIR, exist_ok=True)
    with open(MANIFEST, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"firmware/manifest.json   {json.dumps(manifest)}")

def festivals_only():
    if not os.path.isfile(MANIFEST):
        sys.exit("firmware/manifest.json does not exist yet -- publish a firmware release first.")
    stamp = festival_stamp()
    if not stamp:
        sys.exit("config/festivals.txt not found next to this script.")
    manifest = json.load(open(MANIFEST))
    if manifest.get("festivals") == stamp:
        print("Stamp already current; nothing to do.")
        return
    manifest["festivals"] = stamp
    write_manifest(manifest)
    print("\nCommit config/festivals.txt and firmware/manifest.json together.")

def firmware_release(src, version):
    version = version.strip().lstrip("v")
    parts = version.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        sys.exit(f"Version must look like 5.6.4, got {version!r}")
    if not os.path.isfile(src):
        sys.exit(f"Not found: {src}")
    if src.lower().endswith(".merged.bin") or "bootloader" in src.lower() or "partitions" in src.lower():
        sys.exit("Use the plain Panel_vX.ino.bin -- not merged/bootloader/partitions.")

    data = open(src, "rb").read()
    if len(data) < 200_000 or data[0] != 0xE9:
        sys.exit("That doesn't look like an ESP32 app image (bad size or magic byte).")

    os.makedirs(FW_DIR, exist_ok=True)
    shutil.copyfile(src, os.path.join(FW_DIR, "latest.bin"))
    manifest = {"version": version, "size": len(data), "md5": hashlib.md5(data).hexdigest()}
    stamp = festival_stamp()
    if stamp:
        manifest["festivals"] = stamp
    print(f"firmware/latest.bin      {len(data):,} bytes")
    write_manifest(manifest)
    print("\nCommit firmware/latest.bin and firmware/manifest.json.")

def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--festivals":
        festivals_only()
    elif len(sys.argv) == 3:
        firmware_release(sys.argv[1], sys.argv[2])
    else:
        print(__doc__); sys.exit(1)

if __name__ == "__main__":
    main()
