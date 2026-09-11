// SPDX-License-Identifier: GPL-3.0-or-later
// Bundled, unmodified copy of ../../include/time_utils.hpp from the main
// moon-graze C++ engine, so this directory builds standalone. Keep in sync
// by hand if the original changes.
// moon-graze -- time scale utilities (calendar <-> Julian Date, UTC->TT,
// Greenwich Mean Sidereal Time).
//
// KNOWN SIMPLIFICATION (documented, not hidden): UT1 is approximated by UTC
// (they differ by at most ~0.9s by IERS convention, which the GMST formula
// below turns into at most a few hundred metres of longitude error on the
// ground -- small compared to the mean-limb model's own multi-km
// simplification). Supplying real UT1-UTC from an IERS bulletin would remove
// this term; see README.md.
#pragma once
#include <string>

namespace mg {

struct CalendarUTC {
    int year, month, day, hour, minute;
    double second;
};

// TAI-UTC leap seconds, IERS Bulletin C (public domain). Extend as needed.
double tai_minus_utc(const CalendarUTC& t);
double tt_minus_utc(const CalendarUTC& t);

// Julian Date (Terrestrial Time) for a UTC calendar date/time.
double jd_tt_from_utc(const CalendarUTC& t);

// Julian Date (UTC numerically, i.e. treating UTC seconds as if uniform --
// this is what the GMST formula conventionally wants as "UT1").
double jd_utc_from_utc(const CalendarUTC& t);

// Greenwich Mean Sidereal Time, radians, from a UT1(~UTC) Julian Date.
// Meeus, "Astronomical Algorithms" 2nd ed., eq. 12.4.
double gmst_radians(double jd_ut1);

// Convert a Julian Date expressed in TT back to one expressed in UT1(~UTC),
// by looking up the leap-second offset for that approximate date. Needed
// because the ephemeris and all root-finding iterate on JD_TT, but Earth
// rotation (GMST) is conventionally a function of UT1.
double jd_tt_to_ut1(double jd_tt);

} // namespace mg
