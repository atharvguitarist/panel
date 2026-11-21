#!/usr/bin/env python3
"""
Turns whatever was uploaded into a release the panels can install.
Run by .github/workflows/publish.yml on every push to firmware/ or
config/festivals.txt. Also runnable locally from the repo root.

  firmware/<anything>.bin   -> renamed to firmware/latest.bin
  firmware/latest.bin       -> version read from inside the binary
                               (the firmware embeds "PANELFW:x.y.z"),
                               size + md5 computed
  config/festivals.txt      -> validated; its md5 prefix becomes the
                               manifest's "festivals" stamp, which is what
                               tells panels to fetch the new calendar
  sdcard/<folder>/<file>    -> listed in firmware/assets.txt ("size md5
                               /path"); its md5 prefix becomes the manifest's
                               "assets" stamp. Panels fetch the files that
                               differ from their card. Only the artwork
                               folders below are allowed.
  firmware/manifest.json    -> rewritten only if something changed

Refuses (exit 1, nothing written) on: a non-ESP32 file, two .bin uploads at
once, a binary with no version tag, a different binary re-using the
version already published, or a festival file with errors.
"""
import glob, hashlib, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware")
LATEST = os.path.join(FW, "latest.bin")
MANIFEST = os.path.join(FW, "manifest.json")
FESTIVALS = os.path.join(ROOT, "config", "festivals.txt")
SDCARD = os.path.join(ROOT, "sdcard")
ASSETS = os.path.join(FW, "assets.txt")
# Card folders panels accept from GitHub (keep in step with ASSET_ROOTS in the sketch).
ASSET_ROOTS = ("bg", "festival", "icons", "ui", "weather_day", "weather_night")
ASSET_NAME = re.compile(r"^[A-Za-z0-9_.\-/]+$")
ASSET_MAX = 3_000_000
CI = "GITHUB_ACTIONS" in os.environ

def fail(msg):
    print(("::error::" if CI else "error: ") + msg)
    sys.exit(1)

def note(msg):
    print(("::notice::" if CI else "") + msg)

def version_tuple(v):
    return tuple(int(x) for x in v.split("."))

def main():
    os.makedirs(FW, exist_ok=True)

    # 1. A freshly uploaded binary under any name becomes latest.bin.
    uploads = [p for p in glob.glob(os.path.join(FW, "*.bin")) if os.path.basename(p) != "latest.bin"]
    if len(uploads) > 1:
        fail("more than one .bin uploaded to firmware/ -- upload only the one Panel_vX.ino.bin")
    src = uploads[0] if uploads else None
    if src:
        name = os.path.basename(src).lower()
        if "merged" in name or "bootloader" in name or "partitions" in name:
            fail(f"{os.path.basename(src)} is not the app image -- upload the plain Panel_vX.ino.bin")

    old = {}
    if os.path.isfile(MANIFEST):
        try:
            old = json.load(open(MANIFEST))
        except Exception as e:
            fail(f"firmware/manifest.json is not valid JSON ({e})")
    new = dict(old)

    # 2. Firmware facts, read from the binary itself. Every check runs
    # BEFORE the upload replaces latest.bin, so a rejected upload never
    # touches what is already published.
    binpath = src or (LATEST if os.path.isfile(LATEST) else None)
    if binpath:
        data = open(binpath, "rb").read()
        if len(data) < 200_000 or data[0] != 0xE9:
            fail(f"{os.path.basename(binpath)} is not an ESP32 app image (bad size or magic byte)")
        tags = set(re.findall(rb"PANELFW:(\d+\.\d+\.\d+)", data))
        if len(tags) != 1:
            fail("could not read the version from the binary -- it must be built from a sketch "
                 "that has the PANELFW version tag (5.6.4 or later)")
        version = tags.pop().decode()
        md5 = hashlib.md5(data).hexdigest()
        if old.get("version") == version and old.get("md5") not in (None, md5):
            fail(f"version {version} is already published with a different binary -- "
                 f"bump the number in FW_VERSION_TAG in the sketch, rebuild, and upload again")
        if old.get("version") and version_tuple(version) < version_tuple(old["version"]):
            print(("::warning::" if CI else "warning: ") +
                  f"publishing {version}, older than the {old['version']} it replaces -- "
                  f"panels already on {old['version']} will NOT downgrade")
        new.update({"version": version, "size": len(data), "md5": md5})

    # 3. Festival calendar: validate, then stamp.
    if os.path.isfile(FESTIVALS):
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import validate_festivals
        if validate_festivals.main(FESTIVALS) != 0:
            fail("config/festivals.txt has errors (listed above) -- panels keep their current dates")
        new["festivals"] = hashlib.md5(open(FESTIVALS, "rb").read()).hexdigest()[:8]
    else:
        new.pop("festivals", None)

    # 4. Card files: list, checksum, stamp.
    if os.path.isdir(SDCARD):
        lines = []
        for dirpath, _, files in os.walk(SDCARD):
            for name in sorted(files):
                full = os.path.join(dirpath, name)
                rel = "/" + os.path.relpath(full, SDCARD).replace(os.sep, "/")
                if name.startswith(".") or name.lower() in ("readme.md", "readme.txt"):
                    continue
                top = rel.split("/")[1]
                if top not in ASSET_ROOTS:
                    fail(f"sdcard{rel}: only {', '.join(ASSET_ROOTS)} may be sent to panels "
                         "(never config, logs or firmware)")
                if not ASSET_NAME.match(rel) or ".." in rel or len(rel) >= 64:
                    fail(f"sdcard{rel}: use letters, digits, _ - . only, and keep the path under 64 characters")
                data = open(full, "rb").read()
                if not 0 < len(data) <= ASSET_MAX:
                    fail(f"sdcard{rel}: {len(data):,} bytes -- files must be 1 byte to {ASSET_MAX:,} bytes")
                lines.append(f"{len(data)} {hashlib.md5(data).hexdigest()} {rel}")
        lines.sort(key=lambda l: l.split(" ", 2)[2])
        if len(lines) > 400:
            fail(f"{len(lines)} files under sdcard/ -- panels accept at most 400")
        text = "\n".join(lines) + "\n" if lines else ""
        if lines:
            stamp = hashlib.md5(text.encode()).hexdigest()[:8]
            if new.get("assets") != stamp or not os.path.isfile(ASSETS):
                with open(ASSETS, "w", newline="\n") as f:
                    f.write(text)
                note(f"assets.txt -> {len(lines)} files, stamp {stamp}")
            new["assets"] = stamp
        else:
            new.pop("assets", None)
    else:
        new.pop("assets", None)

    if src:
        if os.path.exists(LATEST):
            os.remove(LATEST)
        if CI:
            subprocess.run(["git", "add", "-A", src], check=True, cwd=ROOT)
            subprocess.run(["git", "mv", "-f", src, LATEST], check=True, cwd=ROOT)
        else:
            os.replace(src, LATEST)
        note(f"{os.path.basename(src)} -> firmware/latest.bin")

    if "version" not in new:
        note("nothing to publish yet (no firmware/latest.bin)")
        changed = False
    else:
        changed = (new != old) or bool(src)
        if changed:
            with open(MANIFEST, "w") as f:
                json.dump(new, f, indent=2)
                f.write("\n")
            note(f"manifest.json -> {json.dumps(new)}")
        else:
            note("manifest already current")

    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a") as f:
            f.write(f"changed={'true' if changed else 'false'}\n")
            f.write(f"version={new.get('version', '')}\n")

if __name__ == "__main__":
    main()
