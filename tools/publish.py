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
