#!/usr/bin/env python3
"""Generate rmq's timezone table from tzdata, verifying every entry.

The TZif footer is NOT usable: for zones with a legislated future change it
describes the rule after the file's last transition, not today's. America/
Vancouver's footer says MST7 while the zone actually observes PST/PDT.

So the POSIX string is computed from the current year's real transitions and
then verified by handing it to the C library and comparing offsets across the
whole year against zoneinfo.
"""
import os, time, datetime as dt
from zoneinfo import ZoneInfo

YEAR = 2026


def off_str(seconds):
    """POSIX offset: west of Greenwich is positive, and it is the negation."""
    s = -seconds
    sign = "-" if s < 0 else ""
    s = abs(s)
    h, rem = divmod(s, 3600)
    m, sec = divmod(rem, 60)
    out = f"{sign}{h}"
    if m or sec:
        out += f":{m:02d}"
    if sec:
        out += f":{sec:02d}"
    return out


def abbr_str(abbr, seconds):
    """An alphabetic abbreviation, or the <+NN> form POSIX requires otherwise.

    The numeric form is the UTC offset the normal way round (east positive),
    which is the opposite sign to the offset field beside it, and zero-padded
    to two digits the way tzdata writes it."""
    if abbr and abbr.isalpha() and len(abbr) >= 3:
        return abbr
    sign = "+" if seconds >= 0 else "-"
    h, rem = divmod(abs(seconds), 3600)
    m = rem // 60
    out = f"{sign}{h:02d}"
    if m:
        out += f"{m:02d}"
    return f"<{out}>"


def rule_str(when):
    """A transition instant as Mm.w.d/h."""
    month = when.month
    dow = (when.weekday() + 1) % 7  # POSIX: 0 = Sunday
    # Which occurrence of this weekday in the month; 5 means "last".
    occ = (when.day - 1) // 7 + 1
    nxt = when.day + 7
    last = (nxt > (dt.date(when.year, month % 12 + 1, 1) - dt.timedelta(days=1)).day)
    if last:
        occ = 5
    out = f"M{month}.{occ}.{dow}"
    if (when.hour, when.minute, when.second) != (2, 0, 0):
        out += f"/{when.hour}"
        if when.minute or when.second:
            out += f":{when.minute:02d}"
    return out


def transitions(zone, year):
    """DST transition instants in local standard-agnostic wall time."""
    z = ZoneInfo(zone)
    out = []
    t = dt.datetime(year, 1, 1, tzinfo=dt.timezone.utc)
    end = dt.datetime(year + 1, 1, 1, tzinfo=dt.timezone.utc)
    prev = t.astimezone(z).dst()
    prev_off = t.astimezone(z).utcoffset()
    step = dt.timedelta(hours=1)
    while t < end:
        t += step
        cur = t.astimezone(z).dst()
        if cur != prev:
            # Narrow to the minute, then express in local wall time.
            lo, hi = t - step, t
            while hi - lo > dt.timedelta(minutes=1):
                mid = lo + (hi - lo) / 2
                if mid.astimezone(z).dst() == prev:
                    lo = mid
                else:
                    hi = mid
            # POSIX gives the transition in the local time in effect just
            # *before* it — standard time for the spring rule, daylight for
            # the autumn one. Converting the instant after the change would
            # read a spring-forward at 02:00 as 03:00.
            before = hi.astimezone(dt.timezone(prev_off))
            out.append((before, cur))
            prev = cur
            prev_off = t.astimezone(z).utcoffset()
    return out


def posix_for(zone, year=YEAR):
    z = ZoneInfo(zone)
    jan = dt.datetime(year, 1, 15, 12, tzinfo=z)
    jul = dt.datetime(year, 7, 15, 12, tzinfo=z)

    std = jan if jan.dst() == dt.timedelta(0) else jul
    if std.dst() != dt.timedelta(0):
        return None  # DST all year: not expressible, and not real
    std_off = int(std.utcoffset().total_seconds())
    s = abbr_str(std.tzname(), std_off) + off_str(std_off)

    tr = transitions(zone, year)
    if not tr:
        return s
    if len(tr) != 2:
        return None  # more than one DST period a year; leave it out

    (t1, d1), (t2, d2) = tr
    start, endt = (t1, t2) if d1 else (t2, t1)
    dst = dt.datetime(year, start.month, start.day, tzinfo=z) + dt.timedelta(days=1)
    dst_off = int(dst.utcoffset().total_seconds())
    s += abbr_str(dst.tzname(), dst_off)
    if dst_off - std_off != 3600:
        s += off_str(dst_off)
    return f"{s},{rule_str(start)},{rule_str(endt)}"


def verify(zone, posix, year=YEAR):
    """Hand the string to the C library and compare a year of offsets."""
    z = ZoneInfo(zone)
    os.environ["TZ"] = posix
    time.tzset()
    t = dt.datetime(year, 1, 1, tzinfo=dt.timezone.utc)
    for _ in range(365 * 24 * 2):
        ts = t.timestamp()
        want = int(t.astimezone(z).utcoffset().total_seconds())
        got = time.localtime(ts).tm_gmtoff
        if want != got:
            return False
        t += dt.timedelta(hours=1)
    return True


zones = []
for line in open("/usr/share/zoneinfo/zone1970.tab"):
    if line.startswith("#") or not line.strip():
        continue
    for name in line.split("\t")[2].strip().split(","):
        zones.append(name)
zones = sorted(set(zones))

ok, bad, shifting = [], [], []
for zone in zones:
    try:
        p = posix_for(zone)
        if p and verify(zone, p):
            ok.append((zone, p))
        else:
            for back in (YEAR - 1, YEAR - 2):
                p = posix_for(zone, back)
                if p and verify(zone, p, back):
                    ok.append((zone, p))
                    shifting.append(zone)
                    break
            else:
                bad.append(zone)
    except Exception as e:
        bad.append(f"{zone}: {e}")

# UTC first, then alphabetical: it is the only sensible default.
ok.sort(key=lambda kv: (kv[0] != "UTC", kv[0]))
if not any(k == "UTC" for k, _ in ok):
    ok.insert(0, ("UTC", "UTC0"))

with open("/home/john/.claude/jobs/d647cc3f/tmp/zones.inc", "w") as f:
    for zone, p in ok:
        f.write(f'\t{{"{zone}", "{p}"}},\n')

print(f"verified {len(ok)} zones, rejected {len(bad)}")
print(f"rules changing soon (described by the rule before the change): {shifting}")
for b in bad[:12]:
    print("  rejected:", b)
for z in ("America/Vancouver", "America/Los_Angeles", "Europe/London", "Asia/Kolkata",
          "Australia/Lord_Howe", "UTC"):
    hit = [p for k, p in ok if k == z]
    print(f"  {z:22} {hit[0] if hit else '(REJECTED)'}")
