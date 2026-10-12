#!/usr/bin/env python3
"""
Checks config/festivals.txt before it can reach any panel.

The firmware applies the same rules, and silently refuses a file that breaks
them -- this script just makes the refusal visible at commit time instead of
at 03:30 on a wall somewhere.

Rules (one entry per line):
    key,YYYY-MM-DD,YYYY-MM-DD        theme key, first day, last day (inclusive)
    # comment                        ignored
    blank lines                      ignored

Exit status 1 on any problem, with a line-numbered reason.
"""
import re, sys
from datetime import date

KNOWN_KEYS = {
    "diwali", "holi", "ganesh_chaturthi", "ganesh", "rakhi", "raksha_bandhan",
    "janmashtami", "krishna_janmashtami", "navratri", "christmas", "halloween",
    "republic", "independence", "valentines", "valentine", "mothers_day",
    "fathers_day", "new_year", "new_year_eve", "winter", "normal",
}
LINE = re.compile(r"^([a-z_]{1,27}),(\d{4}-\d{2}-\d{2}),(\d{4}-\d{2}-\d{2})$")

def main(path):
    problems, entries = [], []
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    if "\t" in raw:
        problems.append("file contains TAB characters; use plain commas")
    for n, line in enumerate(raw.splitlines(), 1):
        line = line.rstrip()
        if not line or line.startswith("#"):
            continue
        if len(line) > 120:
            problems.append(f"line {n}: longer than 120 characters")
            continue
        m = LINE.match(line)
        if not m:
            problems.append(f"line {n}: expected key,YYYY-MM-DD,YYYY-MM-DD  ->  {line!r}")
            continue
        key, a, b = m.groups()
        try:
            da, db = date.fromisoformat(a), date.fromisoformat(b)
        except ValueError as e:
            problems.append(f"line {n}: bad date ({e})")
            continue
        if db < da:
            problems.append(f"line {n}: end date {b} is before start date {a}")
        if (db - da).days > 15:
            problems.append(f"line {n}: {key} spans {(db-da).days+1} days -- check the dates")
        if key not in KNOWN_KEYS:
            problems.append(f"line {n}: unknown key {key!r} (panel would fall back to the normal theme)")
        entries.append((da, db, key, n))

    # Overlaps: the firmware takes the first match, so an overlap is a silent
    # priority decision. Flag it so it is a deliberate one.
    entries.sort()
    for (a1, b1, k1, n1), (a2, b2, k2, n2) in zip(entries, entries[1:]):
        if a2 <= b1 and k1 != k2:
            problems.append(f"lines {n1} and {n2}: {k1} and {k2} overlap ({a2} <= {b1}); the first one in the file wins")

    import os
    ci = "GITHUB_ACTIONS" in os.environ
    this_year = date.today().year
    years = {e[0].year for e in entries}
    if entries and max(years) < this_year + 1:
        print(("::warning::" if ci else "warning: ") + f"no entries beyond {max(years)} -- add next year's dates")

    # The panel only re-fetches the calendar when the manifest's "festivals"
    # stamp changes. A calendar edit without a stamp update would sit in the
    # repo and never reach a single panel -- so a stale stamp is an error.
    import hashlib
    manifest_path = os.path.join(os.path.dirname(os.path.abspath(path)), "..", "firmware", "manifest.json")
    if os.path.isfile(manifest_path):
        import json
        stamp = hashlib.md5(open(path, "rb").read()).hexdigest()[:8]
        try:
            have = json.load(open(manifest_path)).get("festivals", "")
        except Exception as e:
            have = None
            problems.append(f"firmware/manifest.json is not valid JSON ({e})")
        if have is not None and have != stamp:
            problems.append(f"firmware/manifest.json 'festivals' is {have!r} but the file's stamp is {stamp!r} "
                            f"-- run  python make_release.py --festivals  and commit the manifest too")

    for p in problems:
        print(("::error::" if ci else "error: ") + p)
    print(f"{len(entries)} entries, years {min(years) if years else '-'}..{max(years) if years else '-'}")
    return 1 if problems else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "config/festivals.txt"))
