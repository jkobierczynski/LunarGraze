// SPDX-License-Identifier: GPL-3.0-or-later
// Bundled, unmodified copy of ../../src/time_utils.cpp from the main
// moon-graze C++ engine, so this directory builds standalone. Keep in sync
// by hand if the original changes.
#include "time_utils.hpp"
#include "vec3.hpp"
#include <cmath>
#include <vector>

namespace mg {

namespace {
struct LeapEntry { int y, m, d; double secs; };
// Same table as tools/gen_ephemeris.py -- keep both in sync.
const std::vector<LeapEntry> kLeaps = {
    {1972,1,1,10}, {1972,7,1,11}, {1973,1,1,12}, {1974,1,1,13}, {1975,1,1,14},
    {1976,1,1,15}, {1977,1,1,16}, {1978,1,1,17}, {1979,1,1,18}, {1980,1,1,19},
    {1981,7,1,20}, {1982,7,1,21}, {1983,7,1,22}, {1985,7,1,23}, {1988,1,1,24},
    {1990,1,1,25}, {1991,1,1,26}, {1992,7,1,27}, {1993,7,1,28}, {1994,7,1,29},
    {1996,1,1,30}, {1997,7,1,31}, {1999,1,1,32}, {2006,1,1,33}, {2009,1,1,34},
    {2012,7,1,35}, {2015,7,1,36}, {2017,1,1,37},
};

long triple_key(int y, int m, int d) { return (long)y * 10000 + m * 100 + d; }
} // namespace

double tai_minus_utc(const CalendarUTC& t) {
    double val = kLeaps.front().secs;
    long key = triple_key(t.year, t.month, t.day);
    for (auto& e : kLeaps) {
        if (key >= triple_key(e.y, e.m, e.d)) val = e.secs;
        else break;
    }
    return val;
}

double tt_minus_utc(const CalendarUTC& t) {
    return tai_minus_utc(t) + 32.184;
}

namespace {
// Julian Day Number for a proleptic-Gregorian calendar date (no time-of-day).
double jdn(int y, int m, int d) {
    int a = (14 - m) / 12;
    int y2 = y + 4800 - a;
    int m2 = m + 12 * a - 3;
    return d + (153 * m2 + 2) / 5 + 365 * y2 + y2 / 4 - y2 / 100 + y2 / 400 - 32045;
}

double add_seconds_to_jd(const CalendarUTC& t, double extra_seconds) {
    double base_jdn = jdn(t.year, t.month, t.day);
    double day_seconds = t.hour * 3600.0 + t.minute * 60.0 + t.second + extra_seconds;
    double frac = day_seconds / 86400.0 - 0.5;
    return base_jdn + frac;
}
} // namespace

double jd_tt_from_utc(const CalendarUTC& t) {
    return add_seconds_to_jd(t, tt_minus_utc(t));
}

double jd_utc_from_utc(const CalendarUTC& t) {
    return add_seconds_to_jd(t, 0.0);
}

namespace {
// Inverse of jdn(): Julian Day Number -> proleptic Gregorian calendar date.
// Meeus, "Astronomical Algorithms", ch. 7.
void calendar_from_jdn(double jd_at_noon, int& y, int& m, int& d) {
    double jd = jd_at_noon + 0.5;
    long Z = (long)jd;
    double F = jd - Z;
    long A = Z;
    if (Z >= 2299161) {
        long alpha = (long)((Z - 1867216.25) / 36524.25);
        A = Z + 1 + alpha - alpha / 4;
    }
    long B = A + 1524;
    long C = (long)((B - 122.1) / 365.25);
    long D = (long)(365.25 * C);
    long E = (long)((B - D) / 30.6001);
    double day = B - D - (long)(30.6001 * E) + F;
    m = (E < 14) ? (int)(E - 1) : (int)(E - 13);
    y = (m > 2) ? (int)(C - 4716) : (int)(C - 4715);
    d = (int)day;
}
} // namespace

double jd_tt_to_ut1(double jd_tt) {
    // Rough calendar date for a leap-second-table lookup (a fraction-of-a-
    // day error here can only matter within the same instant a leap second
    // is actually inserted, which is never during a real occultation).
    int y, m, d;
    calendar_from_jdn(jd_tt, y, m, d);
    CalendarUTC approx{y, m, d, 0, 0, 0.0};
    double offset_days = tt_minus_utc(approx) / 86400.0;
    return jd_tt - offset_days;
}

double gmst_radians(double jd_ut1) {
    double T = (jd_ut1 - 2451545.0) / 36525.0;
    // Meeus 12.4, degrees
    double gmst_deg = 280.46061837
                     + 360.98564736629 * (jd_ut1 - 2451545.0)
                     + 0.000387933 * T * T
                     - (T * T * T) / 38710000.0;
    double g = std::fmod(gmst_deg, 360.0);
    if (g < 0) g += 360.0;
    return deg2rad(g);
}

} // namespace mg
