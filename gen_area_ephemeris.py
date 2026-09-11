#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
gen_area_ephemeris.py -- bake a Moon+Sun geocentric ephemeris table (JPL
DE421) for graze_gui, the C++/Qt area-scan grazing-occultation tool.

WHY A SEPARATE BAKER FROM tools/gen_ephemeris.py
--------------------------------------------------
The original moon-graze C++ engine (../../src) bakes short (few-hour),
high-cadence (60s step) tables around ONE already-known event, and only
needs the Moon (for DEM-limb flash timing, in RA/Dec tangent-plane
coordinates -- it never needs the Sun or a horizon altitude). graze_gui
instead needs to *search* an arbitrary date range (weeks to years) for
candidate events across the whole sky, and needs the Sun's position too
(for Sun-altitude "is it dark" filtering and the Moon's illuminated
fraction) -- different enough inputs/outputs to warrant its own baker and
its own on-disk table format, rather than overloading the original one.

Empirically (see project chat history), 4-point Lagrange interpolation of
DE421 Moon/Sun positions -- the same scheme ../../src/ephemeris.cpp uses --
has sub-milliarcsecond error even at a 2-hour bake step, because both
bodies move smoothly on that timescale. This script defaults to a 60-minute
step, i.e. ~60x more headroom than that test showed necessary, while still
keeping a several-year table to a few MB.

DATA PROVENANCE
----------------
Same as tools/gen_ephemeris.py: JPL DE421 (NASA/JPL, public domain) via the
BSD-licensed `jplephem` package and the public-domain `de421` data package.
If `pip install de421` fails to build (old-setuptools sdist issue), do:
    pip download --no-deps -d /tmp/de421pkg de421
    tar xzf /tmp/de421pkg/de421-*.tar.gz -C /tmp/de421pkg
    export PYTHONPATH=/tmp/de421pkg/de421-<version>
and re-run this script.

OUTPUT FORMAT
-------------
    # comment lines (start with '#')
    <N>
    <N rows of>: jd_tt  moon_x moon_y moon_z  sun_x sun_y sun_z  \
                 earthvel_x earthvel_y earthvel_z
All positions are geocentric, ICRF-equatorial J2000, km; earthvel is the
Earth's SSB velocity, km/s (for stellar aberration, applied C++-side by the
existing, unmodified mg::Star::apply_aberration). This 10-column format is
read by graze_gui's AreaEphemeris loader (src/areaephem.cpp) -- it is a
different (simpler) format than ../../src/ephemeris.cpp's 9-column
(no-Sun) one, so tables from the two scripts are NOT interchangeable.

USAGE
-----
    python3 gen_area_ephemeris.py --start 2025-01-01 --end 2029-01-01 \\
        --step-minutes 60 --out data/area_eph_2025_2029.txt
"""
import argparse
import datetime as dt
import sys

from jplephem import Ephemeris
try:
    import de421
except ImportError:
    print(
        "Could not import the 'de421' package -- see this script's header "
        "docstring for the pip-download workaround.", file=sys.stderr,
    )
    raise
import numpy as np

EMRAT = 81.30056  # Earth/Moon mass ratio, DE421 constant

# IERS leap-second table (TAI-UTC, seconds), public domain (IERS Bulletin C).
# Identical to tools/gen_ephemeris.py's table -- extend both if baking past
# the last entry's validity (a missing future leap second costs at most 1s,
# i.e. ~0.5 arcsec of Moon position -- utterly negligible for this tool's
# km-scale line-tracing precision, but easy to keep in sync regardless).
LEAP_SECONDS = [
    (dt.datetime(1972, 1, 1), 10), (dt.datetime(1972, 7, 1), 11),
    (dt.datetime(1973, 1, 1), 12), (dt.datetime(1974, 1, 1), 13),
    (dt.datetime(1975, 1, 1), 14), (dt.datetime(1976, 1, 1), 15),
    (dt.datetime(1977, 1, 1), 16), (dt.datetime(1978, 1, 1), 17),
    (dt.datetime(1979, 1, 1), 18), (dt.datetime(1980, 1, 1), 19),
    (dt.datetime(1981, 7, 1), 20), (dt.datetime(1982, 7, 1), 21),
    (dt.datetime(1983, 7, 1), 22), (dt.datetime(1985, 7, 1), 23),
    (dt.datetime(1988, 1, 1), 24), (dt.datetime(1990, 1, 1), 25),
    (dt.datetime(1991, 1, 1), 26), (dt.datetime(1992, 7, 1), 27),
    (dt.datetime(1993, 7, 1), 28), (dt.datetime(1994, 7, 1), 29),
    (dt.datetime(1996, 1, 1), 30), (dt.datetime(1997, 7, 1), 31),
    (dt.datetime(1999, 1, 1), 32), (dt.datetime(2006, 1, 1), 33),
    (dt.datetime(2009, 1, 1), 34), (dt.datetime(2012, 7, 1), 35),
    (dt.datetime(2015, 7, 1), 36), (dt.datetime(2017, 1, 1), 37),
]


def tai_minus_utc(d):
    val = LEAP_SECONDS[0][1]
    for (eff, secs) in LEAP_SECONDS:
        if d >= eff:
            val = secs
        else:
            break
    return val


def tt_minus_utc(d):
    return tai_minus_utc(d) + 32.184  # TT = TAI + 32.184s exactly


def jd_from_datetime_utc(d):
    tt = d + dt.timedelta(seconds=tt_minus_utc(d))
    y, m, day = tt.year, tt.month, tt.day
    a = (14 - m) // 12
    y2 = y + 4800 - a
    m2 = m + 12 * a - 3
    jdn = day + (153 * m2 + 2) // 5 + 365 * y2 + y2 // 4 - y2 // 100 + y2 // 400 - 32045
    frac = (tt.hour + tt.minute / 60.0 + tt.second / 3600.0 + tt.microsecond / 3.6e9) / 24.0 - 0.5
    return jdn + frac


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--start", required=True, help="UTC start, e.g. 2025-01-01")
    ap.add_argument("--end", required=True, help="UTC end, e.g. 2029-01-01")
    ap.add_argument("--step-minutes", type=float, default=60.0,
                     help="bake interval (default 60 min; see header docstring "
                          "for why this has huge interpolation-error headroom)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    t0 = dt.datetime.fromisoformat(args.start)
    t1 = dt.datetime.fromisoformat(args.end)
    step = dt.timedelta(minutes=args.step_minutes)

    eph = Ephemeris(de421)
    if not (eph.jalpha <= jd_from_datetime_utc(t0) and jd_from_datetime_utc(t1) <= eph.jomega):
        print(f"warning: requested range may fall outside DE421 coverage "
              f"(JD {eph.jalpha:.1f}..{eph.jomega:.1f})", file=sys.stderr)

    rows = []
    t = t0
    dtau = 1.0 / 86400.0  # 1 second in days, for finite-difference velocity
    n_total = int((t1 - t0) / step) + 1
    while t <= t1:
        jd = jd_from_datetime_utc(t)

        moon = eph.position("moon", jd).flatten()      # geocentric, km, ICRF equatorial
        emb = eph.position("earthmoon", jd).flatten()
        earth = emb - moon * (1.0 / (1.0 + EMRAT))      # Earth's SSB position, km
        sun_bary = eph.position("sun", jd).flatten()
        sun = sun_bary - earth                           # geocentric Sun position, km

        emb_p = eph.position("earthmoon", jd + dtau).flatten()
        moon_p = eph.position("moon", jd + dtau).flatten()
        earth_p = emb_p - moon_p * (1.0 / (1.0 + EMRAT))
        earth_vel = (earth_p - earth) / (dtau * 86400.0)  # km/s

        rows.append((jd, *moon, *sun, *earth_vel))
        if len(rows) % 5000 == 0:
            print(f"  ...{len(rows)}/{n_total} rows", file=sys.stderr)
        t += step

    with open(args.out, "w") as f:
        f.write("# graze_gui baked area ephemeris table (DE421, ICRF equatorial J2000, km, km/s)\n")
        f.write("# columns: JD_TT  moon_x moon_y moon_z  sun_x sun_y sun_z  earthvel_x earthvel_y earthvel_z\n")
        f.write("# generated by gen_area_ephemeris.py -- see file header for data provenance\n")
        f.write(f"# range: {args.start} to {args.end} UTC, step {args.step_minutes} min\n")
        f.write(f"{len(rows)}\n")
        for r in rows:
            f.write(" ".join(f"{v:.9f}" for v in r) + "\n")

    print(f"wrote {len(rows)} rows ({t0.date()}..{t1.date()}) to {args.out}")


if __name__ == "__main__":
    main()
