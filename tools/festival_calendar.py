#!/usr/bin/env python3
"""
Writes config/festivals.txt -- the lunisolar festival calendar -- from the
positions of the Sun and Moon. Runs in GitHub Actions twice a year; the panel
never does any of this maths, it only downloads the finished text file when
its stamp in manifest.json changes.

How the dates are found (the same rules the printed panchangs use):

  tithi     the lunar day: every 12 degrees the Moon gains on the Sun.
  month     amanta (new moon to new moon), named by the sidereal sign the
            Sun is in when it starts (Lahiri ayanamsa). A month with no
            sankranti in it is adhik (extra) -- no festival falls in one.
  the day   a festival belongs to the civil day on which its tithi holds
            the part of the day the rite is done in (its karmakala):
              Diwali        Ashvin amavasya         pradosh (after sunset)
              Bhai Dooj     Kartika shukla 2        aparahna (afternoon)
              Holika Dahan  Phalguna purnima        pradosh; Holi is the next day
              Rakhi         Shravana purnima        aparahna
              Janmashtami   Shravana krishna 8      nishita (midnight) and sunrise
              Ganesh        Bhadrapada shukla 4     madhyahna (midday)
              Navratri      Ashvin shukla 1         sunrise; ends the day before
              Dussehra      Ashvin shukla 10        aparahna
            When the tithi touches that window on two days, the day it
            covers more of wins. Bhadra moves Holika Dahan and Rakhi in the
            years it should (rules beside each festival below), and
            Janmashtami covers both the Smarta and the ISKCON day.

Location: New Delhi (dates for India). Overrides: any line in
config/festivals_overrides.txt replaces the computed entry of the same key
within 20 days of it -- a one-line fix if a year ever needs one.

    python3 tools/festival_calendar.py                 # writes config/festivals.txt
    python3 tools/festival_calendar.py --selftest      # reproduce every published reference date
    python3 tools/festival_calendar.py --check FILE    # compare against another reference list
"""
import argparse, math, os, sys
from datetime import date, datetime, timedelta, timezone

from zoneinfo import ZoneInfo

import ephem

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "config", "festivals.txt")
OVERRIDES = os.path.join(ROOT, "config", "festivals_overrides.txt")

YEARS_AHEAD = 10              # this year + 9 more: ~2 KB, far under the panel's 12 KB cap
LAT, LON, TZ_NAME = "28.6139", "77.2090", "Asia/Kolkata"   # New Delhi

MASA = ["chaitra", "vaishakha", "jyeshtha", "ashadha", "shravana", "bhadrapada",
        "ashvin", "kartika", "margashirsha", "pausha", "magha", "phalguna"]

# ---------------------------------------------------------------- astronomy
def _lon(body, t):
    body.compute(t)
    eq = ephem.Equatorial(body.g_ra, body.g_dec, epoch=t)
    return math.degrees(ephem.Ecliptic(eq, epoch=t).lon)

_SUN, _MOON = ephem.Sun(), ephem.Moon()

def elongation(t):
    """Moon minus Sun, 0..360 degrees (tropical; the ayanamsa cancels)."""
    return (_lon(_MOON, t) - _lon(_SUN, t)) % 360.0

def lahiri(t):
    """Lahiri ayanamsa in degrees (23 deg 51' 11.7\" at J2000, 50.29\"/year)."""
    years = (float(t) - float(ephem.Date("2000/1/1 12:00"))) / 365.25
    return 23.853242 + years * 50.2888 / 3600.0

def sidereal_sun(t):
    return (_lon(_SUN, t) - lahiri(t)) % 360.0

def nakshatra(t):
    """Lunar mansion 0..26 (3 = Rohini) from the Moon's sidereal longitude."""
    return int(((_lon(_MOON, t) - lahiri(t)) % 360.0) // (360.0 / 27))

def _solve(f, target, lo, hi):
    """Time in [lo, hi] where the unwrapped angle f crosses target (bisection)."""
    for _ in range(60):
        mid = (lo + hi) / 2
        if f(mid) < target:
            lo = mid
        else:
            hi = mid
    return ephem.Date((lo + hi) / 2)

def new_moons(start, end):
    out, t = [], ephem.previous_new_moon(start)
    while t < end:
        out.append(_solve(lambda x: (elongation(x) + 180) % 360 - 180, 0.0, t - 0.5, t + 0.5))
        t = ephem.next_new_moon(t + 1)
    out.append(_solve(lambda x: (elongation(x) + 180) % 360 - 180, 0.0, t - 0.5, t + 0.5))
    return out

# ---------------------------------------------------------------- calendar
TZ = ZoneInfo(TZ_NAME)

def to_local(t):
    return ephem.Date(t).datetime().replace(tzinfo=timezone.utc).astimezone(TZ)

def local_midnight(d):
    """ephem.Date of 00:00 local time on civil date d."""
    utc = datetime(d.year, d.month, d.day, tzinfo=TZ).astimezone(timezone.utc)
    return ephem.Date(utc.replace(tzinfo=None))

_obs = ephem.Observer()
_obs.lat, _obs.lon = LAT, LON
_obs.pressure, _obs.horizon = 0, "-0:34"          # upper limb, standard refraction

def sun_times(d):
    """(sunrise, sunset, next sunrise) for civil date d, as ephem dates."""
    _obs.date = local_midnight(d)
    rise = _obs.next_rising(_SUN)
    sset = _obs.next_setting(_SUN)
    _obs.date = local_midnight(d + timedelta(days=1))
    nxt = _obs.next_rising(_SUN)
    return rise, sset, nxt

def window(d, kind):
    """The karmakala of civil date d as (start, end) ephem dates."""
    rise, sset, nxt = sun_times(d)
    day = sset - rise
    if kind == "sunrise":   return rise, rise + 1e-6
    if kind == "madhyahna": return rise + day * 2 / 5, rise + day * 3 / 5
    if kind == "aparahna":  return rise + day * 3 / 5, rise + day * 4 / 5
    if kind == "pradosh":   return sset, sset + 0.1                       # 2 h 24 min
    if kind == "nishita":
        mid = (sset + nxt) / 2
        return mid - 1 / 60, mid + 1 / 60                                 # 48-min muhurta
    raise ValueError(kind)

class Month:
    def __init__(self, start, end):
        self.start, self.end = start, end
        r0 = int(sidereal_sun(start) // 30)
        r1 = int(sidereal_sun(end) // 30)
        self.masa = (r0 + 1) % 12
        self.adhik = (r0 == r1)

    def tithi_span(self, n):
        """Start and end (ephem dates) of tithi n (1..30) in this month."""
        def f(x):                         # elongation unwrapped across this month
            e = elongation(x)
            if x < self.start + 2 and e > 300: e -= 360
            if x > self.end - 2 and e < 60:    e += 360
            return e
        a =self.start if n == 1 else _solve(f, (n - 1) * 12.0, self.start, self.end)
        b = self.end if n == 30 else _solve(f, n * 12.0, self.start, self.end)
        return a, b

    def bhadra_end(self):
        """Bhadra (vishti karana) fills the first half of purnima: until 174 deg."""
        a, b = self.tithi_span(15)
        f = lambda x: elongation(x)
        return _solve(f, 174.0, a, b)

def months(year_from, year_to):
    nms = new_moons(ephem.Date(f"{year_from - 1}/12/1"), ephem.Date(f"{year_to + 1}/1/31"))
    return [Month(a, b) for a, b in zip(nms, nms[1:])]

def find_month(ms, masa, year):
    """The nija (not adhik) month called `masa` whose tithis fall in `year`."""
    for m in ms:
        if m.masa == masa and not m.adhik and to_local(m.start).year == year:
            return m
    # A month that starts in late December still belongs to the next year's festival.
    for m in ms:
        if m.masa == masa and not m.adhik and to_local(m.end).year == year:
            return m
    raise LookupError(f"{MASA[masa]} {year}")

def observe(month, tithi, kind, tie_later=False):
    """Civil date on which `tithi` of `month` holds the `kind` window best.
    A tie (the tithi fills the window on two days) goes to the first day,
    or to the second when tie_later is set."""
    a, b = month.tithi_span(tithi)
    first = to_local(a).date() - timedelta(days=1)
    best, best_cover = None, 0.0
    for i in range(4):
        d = first + timedelta(days=i)
        w0, w1 = window(d, kind)
        if kind == "sunrise":
            cover = 1.0 if a <= w0 < b else 0.0
        else:
            cover = max(0.0, min(b, w1) - max(a, w0)) / (w1 - w0)
        if cover > best_cover + 1e-9 or (tie_later and cover > 0.999 and best_cover > 0.999):
            best, best_cover = d, cover
    if best is None:
        # The tithi never meets the window (it starts after one and ends before
        # the next). A night rite stays on the night the tithi begins; a day
        # rite goes to the day the tithi holds at sunrise.
        best = to_local(a).date()
        if kind in ("madhyahna", "aparahna"):
            for i in range(3):
                d = best + timedelta(days=i)
                if a <= sun_times(d)[0] < b:
                    best = d
                    break
    return best

# ---------------------------------------------------------------- festivals
def festivals_for(year, ms):
    """{key: (first_day, last_day)} for one Gregorian year, plus extras for the report."""
    out, info = {}, {}
    ashvin, kartika = find_month(ms, 6, year), None
    for m in ms:
        if m.masa == 7 and not m.adhik and m.start >= ashvin.end - 1:
            kartika = m
            break

    lakshmi = observe(ashvin, 30, "pradosh")
    # Bhai Dooj ends the Diwali run. Afternoon rule and sunrise rule can
    # differ by a day (2022, 2023); the theme stays up through the later one.
    bhai = observe(kartika, 2, "aparahna")
    bhai_udaya = observe(kartika, 2, "sunrise")
    info["dhanteras"] = observe(ashvin, 28, "pradosh")
    info["diwali"], info["bhai_dooj"] = lakshmi, bhai
    out["diwali"] = (lakshmi, max(bhai, bhai_udaya))

    # Holika Dahan: purnima at pradosh. If bhadra spoils that evening and
    # purnima runs three and a half prahars into the next day, the fire moves
    # to the next evening (2023, 2026); otherwise it is lit that night after
    # or around bhadra (2024, 2025, 2027).
    phalguna = find_month(ms, 11, year)
    holika = observe(phalguna, 15, "pradosh")
    p_start, p_end = phalguna.tithi_span(15)
    rise, sset, _ = sun_times(holika)
    rise2, sset2, _ = sun_times(holika + timedelta(days=1))
    if (p_start <= sset + 0.1 and phalguna.bhadra_end() > sset
            and p_end - rise2 >= 3.5 * (sset2 - rise2) / 4):
        holika += timedelta(days=1)
    info["holika_dahan"] = holika
    out["holi"] = (holika + timedelta(days=1),) * 2

    shravana = find_month(ms, 4, year)
    # Rakhi: purnima at aparahna, never in bhadra. If bhadra spoils that
    # afternoon and purnima holds 6 ghatis (2 h 24 min) past the next sunrise,
    # the next morning is the day (2025, 2026); otherwise after bhadra (2023).
    rakhi = observe(shravana, 15, "aparahna")
    _, p_end = shravana.tithi_span(15)
    ap0, _ = window(rakhi, "aparahna")
    rise2, _, _ = sun_times(rakhi + timedelta(days=1))
    if shravana.bhadra_end() > ap0 and p_end - rise2 >= 0.1:
        rakhi += timedelta(days=1)
    out["rakhi"] = (rakhi, rakhi)

    # Janmashtami: Smarta (midnight) and Vaishnava (sunrise) can differ by a day.
    # The theme covers both, so whichever day a family keeps, the panel is right.
    # Smarta: ashtami at midnight; it moves to the next day when ashtami is
    # still there at that sunrise and Rohini rules the next midnight (2027,
    # 2029, 2030, 2032). Vaishnava/ISKCON: ashtami at sunrise.
    j1 = observe(shravana, 23, "nishita", tie_later=True)
    j2 = observe(shravana, 23, "sunrise")
    a8, b8 = shravana.tithi_span(23)
    nxt = j1 + timedelta(days=1)
    w0, w1 = window(nxt, "nishita")
    if a8 <= sun_times(nxt)[0] < b8 and nakshatra((w0 + w1) / 2) == 3:
        j1 = nxt
    info["janmashtami_smarta"], info["janmashtami_sunrise"] = j1, j2
    out["janmashtami"] = (min(j1, j2), max(j1, j2))

    ganesh = observe(find_month(ms, 5, year), 4, "madhyahna")
    out["ganesh_chaturthi"] = (ganesh, ganesh)

    ghatasthapana = observe(ashvin, 1, "sunrise")
    dussehra = observe(ashvin, 10, "aparahna")
    info["ghatasthapana"], info["dussehra"] = ghatasthapana, dussehra
    out["navratri"] = (ghatasthapana, dussehra - timedelta(days=1))
    return out, info

# Where each festival can possibly fall (month, day) -- a computed date outside
# its window means a bug or a broken library, never a real calendar quirk, so
# the run stops before anything reaches a panel.
SANE = {
    "holi":             ((2, 20), (3, 31)),
    "rakhi":            ((7, 20), (8, 31)),
    "janmashtami":      ((8, 1),  (9, 10)),
    "ganesh_chaturthi": ((8, 15), (9, 25)),
    "navratri":         ((9, 15), (10, 25)),
    "diwali":           ((10, 15), (11, 20)),
}
SPAN = {"holi": (1, 1), "rakhi": (1, 1), "janmashtami": (1, 2), "ganesh_chaturthi": (1, 1),
        "navratri": (8, 10), "diwali": (3, 5)}

def sanity(rows):
    problems = []
    for a, b, k in rows:
        lo, hi = SANE[k]
        if not (date(a.year, *lo) <= a <= date(a.year, *hi)):
            problems.append(f"{k} starts {a}, outside {lo}..{hi}")
        days = (b - a).days + 1
        least, most = SPAN[k]
        if not least <= days <= most:
            problems.append(f"{k} {a}..{b} lasts {days} days, expected {least}..{most}")
    return problems

def compute(first_year, last_year):
    ms = months(first_year, last_year)
    rows, infos = [], {}
    for y in range(first_year, last_year + 1):
        fest, info = festivals_for(y, ms)
        infos[y] = {**info, **{k: v[0] for k, v in fest.items()}}
        for k, (a, b) in fest.items():
            rows.append((a, b, k))
    rows.sort()
    return rows, infos, [m for m in ms if m.adhik]

# ---------------------------------------------------------------- output
def read_entries(path):
    """(first, last, key) tuples from a festivals file; comments skipped."""
    rows = []
    if not os.path.isfile(path):
        return rows
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            k, a, b = line.split(",")
            rows.append((date.fromisoformat(a), date.fromisoformat(b), k))
        except ValueError:
            sys.exit(f"{os.path.relpath(path, ROOT)} line {n}: expected key,YYYY-MM-DD,YYYY-MM-DD")
    return rows

def render(rows, first_year, last_year):
    overrides = read_entries(OVERRIDES)
    kept = [r for r in rows
            if not any(o[2] == r[2] and abs((o[0] - r[0]).days) <= 20 for o in overrides)]
    final = sorted(overrides + kept)
    for (a1, b1, k1), (a2, b2, k2) in zip(final, final[1:]):
        if a2 <= b1:
            sys.exit(f"overlap: {k1} {a1}..{b1} and {k2} {a2}..{b2}")
    lines = [
        "# Festival calendar for Panel -- written by tools/festival_calendar.py.",
        f"# Lunisolar dates for India (New Delhi sunrise), {first_year}-{last_year}.",
        "# Fixed-date days (New Year, Christmas, Republic Day...) live in the firmware.",
        "# To correct a date, add a line to config/festivals_overrides.txt instead of",
        "# editing this file: the next run would overwrite an edit here.",
        "# key,first day,last day",
    ]
    lines += [f"{k},{a.isoformat()},{b.isoformat()}" for a, b, k in final]
    return final, "\n".join(lines) + "\n"

def moved_soon(old, new, today):
    """Entries in the published file within the next year that the new file
    no longer has unchanged. Panels may already be counting down to them."""
    keep = set(new)
    horizon = today + timedelta(days=366)
    return [e for e in old if today <= e[0] < horizon and e not in keep]

def check(ref_path, infos):
    """Reference file lines: year,event,YYYY-MM-DD[,source]. Prints every mismatch."""
    total = bad = 0
    for line in open(ref_path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(",")
        y, ev, want = int(parts[0]), parts[1], date.fromisoformat(parts[2])
        if y not in infos or ev not in infos[y]:
            continue
        got = infos[y][ev]
        total += 1
        if got != want:
            bad += 1
            print(f"MISMATCH {y} {ev}: computed {got}, reference {want}  ({','.join(parts[3:])})")
    print(f"{total - bad}/{total} reference dates match")
    return bad

def set_place(lat, lon, zone):
    global TZ
    _obs.lat, _obs.lon, TZ = lat, lon, ZoneInfo(zone)

def selftest():
    """Every date in tools/festival_reference*.txt, computed and compared."""
    here = os.path.dirname(os.path.abspath(__file__))
    groups = {}
    for name in ("festival_reference.txt", "festival_reference_abroad.txt"):
        for line in open(os.path.join(here, name), encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            place, _, rest = line.rpartition("|")
            groups.setdefault(place or f"{LAT},{LON},{TZ_NAME}", []).append(rest)
    bad = total = 0
    for place, lines in groups.items():
        set_place(*place.split(","))
        years = sorted({int(l.split(",")[0]) for l in lines})
        _, infos, _ = compute(years[0], years[-1])
        for l in lines:
            y, ev, want = l.split(",")[:3]
            got = infos.get(int(y), {}).get(ev)
            total += 1
            if got is None or got.isoformat() != want:
                bad += 1
                print(f"MISMATCH {place} {y} {ev}: computed {got}, published {want}")
    set_place(LAT, LON, TZ_NAME)
    print(f"self-test: {total - bad}/{total} published dates reproduced")
    return bad == 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--first", type=int, default=date.today().year)
    ap.add_argument("--years", type=int, default=YEARS_AHEAD)
    ap.add_argument("--check", metavar="REFERENCE", help="compare with a list of published dates")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--allow-changes", action="store_true",
                    help="let a date in the next 12 months move (normally refused)")
    ap.add_argument("--place", help="lat,lon,zone -- for testing against other calendars")
    ap.add_argument("--selftest", action="store_true", help="reproduce every published reference date")
    args = ap.parse_args()
    if args.selftest:
        sys.exit(0 if selftest() else 1)
    if args.place:
        set_place(*args.place.split(","))
    last = args.first + args.years - 1
    rows, infos, adhik = compute(args.first, last)
    for m in adhik:
        print(f"adhik {MASA[m.masa]}: {to_local(m.start).date()} .. {to_local(m.end).date()}")
    if args.check:
        sys.exit(1 if check(args.check, infos) else 0)

    problems = sanity(rows)
    if problems:
        sys.exit("refusing to write festivals.txt:\n  " + "\n  ".join(problems))
    final, text = render(rows, args.first, last)
    old_text = open(args.out, encoding="utf-8").read() if os.path.isfile(args.out) else None
    changed = old_text != text
    if changed and old_text is not None:
        moved = moved_soon(read_entries(args.out), final, date.today())
        for a, b, k in moved:
            print(f"{'note' if args.allow_changes else 'error'}: published {k},{a},{b} would change")
        if moved and not args.allow_changes:
            sys.exit("a date panels may already show would move -- check it, then run with "
                     "--allow-changes (or add the date to config/festivals_overrides.txt)")
    if changed:
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"festivals.txt written: {len(final)} entries, {len(text)} bytes")
    else:
        print("festivals.txt unchanged")
    gh = os.environ.get("GITHUB_OUTPUT")
    if gh:
        with open(gh, "a") as f:
            f.write(f"changed={'true' if changed else 'false'}\n")

if __name__ == "__main__":
    main()
