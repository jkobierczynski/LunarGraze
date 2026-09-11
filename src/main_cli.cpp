// SPDX-License-Identifier: GPL-3.0-or-later
// graze_core_test -- headless CLI harness for graze_core, deliberately
// mirroring tools/graze_finder/graze_finder.py's CLI output so the two can
// be diffed directly against each other as a correctness check for this
// C++ port (see project chat history / README.md for the cross-check
// results against both the Python tool and the printed Sterrengids 2026
// almanac).
#include "grazecore.hpp"
#include "starcatalog.hpp"
#include "areaephem.hpp"
#include "time_utils.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <iostream>

namespace {

double jd_tt_from_ymd(int y, int m, int d, double hour = 0.0) {
    mg::CalendarUTC t{y, m, d, 0, 0, hour * 3600.0};
    return mg::jd_tt_from_utc(t);
}

bool parse_ymd(const std::string& s, int& y, int& m, int& d) {
    return std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3;
}

// Render a JD_TT back to a UTC calendar string ("YYYY/M/D H:MM:SS") for
// display, via the standard Meeus ("Astronomical Algorithms" 2nd ed., ch.
// 7) Julian-Date-to-Gregorian-calendar algorithm -- operates directly on
// the JD number, so no need to invert jd_tt_from_utc by search.
std::string jd_to_string(double jd_tt) {
    // Approximate UTC Julian date: subtract the constant TT-UTC offset (37
    // leap seconds + 32.184s, effective since 2017-01-01 and unchanged
    // since, per time_utils.cpp's LEAP_SECONDS table -- fine for this
    // tool's practical date range) to display UTC instead of TT, matching
    // what graze_finder.py's "... UT" column shows.
    double jd_utc = jd_tt - 69.184 / 86400.0;

    double jd = jd_utc + 0.5;
    long Z = static_cast<long>(std::floor(jd));
    double F = jd - Z;
    long A;
    if (Z < 2299161) {
        A = Z;
    } else {
        long alpha = static_cast<long>(std::floor((Z - 1867216.25) / 36524.25));
        A = Z + 1 + alpha - alpha / 4;
    }
    long B = A + 1524;
    long C = static_cast<long>(std::floor((B - 122.1) / 365.25));
    long D = static_cast<long>(std::floor(365.25 * C));
    long E = static_cast<long>(std::floor((B - D) / 30.6001));
    double day_frac = B - D - std::floor(30.6001 * E) + F;
    int day = static_cast<int>(day_frac);
    double hh = (day_frac - day) * 24.0;
    long month = (E < 14) ? (E - 1) : (E - 13);
    long year = (month > 2) ? (C - 4716) : (C - 4715);
    int hour = static_cast<int>(hh);
    double mm = (hh - hour) * 60.0;
    int minute = static_cast<int>(mm);
    double ss = (mm - minute) * 60.0;
    if (ss >= 59.9995) { ss = 0.0; minute += 1; } // avoid printing ":60"
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%ld/%ld/%d %02d:%02d:%02d", year, month, day, hour, minute,
                  static_cast<int>(ss + 0.5));
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    gg::Site site;
    double radius_km = 100.0, mag_limit = 4.0;
    std::string start_s, end_s, catalog_path, eph_path, csv_out;
    bool have_lat = false, have_lon = false, have_start = false, have_end = false, have_eph = false;
    gg::Filters filt;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--lat") { site.lat_deg = std::stod(next()); have_lat = true; }
        else if (a == "--lon") { site.lon_deg = std::stod(next()); have_lon = true; }
        else if (a == "--elevation") { site.elevation_m = std::stod(next()); }
        else if (a == "--radius-km") { radius_km = std::stod(next()); }
        else if (a == "--mag-limit") { mag_limit = std::stod(next()); }
        else if (a == "--start") { start_s = next(); have_start = true; }
        else if (a == "--end") { end_s = next(); have_end = true; }
        else if (a == "--catalog") { catalog_path = next(); }
        else if (a == "--eph") { eph_path = next(); have_eph = true; }
        else if (a == "--csv-out") { csv_out = next(); }
        else if (a == "--min-moon-alt") { filt.filter_min_moon_alt = true; filt.min_moon_alt_deg = std::stod(next()); }
        else if (a == "--max-sun-alt") { filt.filter_max_sun_alt = true; filt.max_sun_alt_deg = std::stod(next()); }
        else if (a == "--min-illum") { filt.filter_min_illum = true; filt.min_illum = std::stod(next()); }
        else if (a == "--max-illum") { filt.filter_max_illum = true; filt.max_illum = std::stod(next()); }
        else if (a == "-v" || a == "--verbose") { verbose = true; }
        else { std::cerr << "unknown arg: " << a << "\n"; return 2; }
    }
    if (!have_lat || !have_lon || !have_start || !have_end || !have_eph) {
        std::cerr << "usage: graze_core_test --lat D --lon D --start Y-M-D --end Y-M-D "
                     "--eph FILE [--catalog FILE] [--radius-km N] [--mag-limit N] "
                     "[--min-moon-alt D] [--max-sun-alt D] [--min-illum F] [--max-illum F] [-v]\n";
        return 2;
    }
    filt.radius_km = radius_km;
    filt.mag_limit = mag_limit;

    int y0, m0, d0, y1, m1, d1;
    if (!parse_ymd(start_s, y0, m0, d0) || !parse_ymd(end_s, y1, m1, d1)) {
        std::cerr << "bad date (want YYYY-MM-DD)\n"; return 2;
    }
    double jd_start = jd_tt_from_ymd(y0, m0, d0);
    double jd_end = jd_tt_from_ymd(y1, m1, d1);

    try {
        gg::AreaEphemeris eph(eph_path);
        auto stars = gg::load_hyg_catalog(catalog_path, mag_limit);
        if (verbose)
            std::cerr << "[graze_core_test] " << stars.size() << " candidate stars (V<=" << mag_limit
                      << ", |eclLat|<" << gg::kEclipticLatCutoffDeg << " deg)\n";

        auto events = gg::find_grazes(site, eph, stars, jd_start, jd_end, filt,
            [&](int done, int total) {
                if (verbose && (done % 20 == 0 || done == total))
                    std::cerr << "[graze_core_test] " << done << "/" << total << " stars scanned\n";
                return true;
            },
            [&](const std::string& msg) { if (verbose) std::cerr << "[graze_core_test] " << msg << "\n"; });

        std::vector<std::string> vis_bits;
        if (filt.filter_min_moon_alt) vis_bits.push_back("Moon above " + std::to_string((int)filt.min_moon_alt_deg) + " deg");
        if (filt.filter_max_sun_alt) vis_bits.push_back("Sun below " + std::to_string((int)filt.max_sun_alt_deg) + " deg");
        std::string vis_note;
        if (!vis_bits.empty()) {
            vis_note = " with ";
            for (size_t i = 0; i < vis_bits.size(); i++) { if (i) vis_note += " and "; vis_note += vis_bits[i]; }
        }
        std::printf("%zu grazing occultation(s) of V<=%.1f stars within %.0f km of (%.4f, %.4f) "
                    "between %s and %s%s:\n\n",
                    events.size(), mag_limit, radius_km, site.lat_deg, site.lon_deg,
                    start_s.c_str(), end_s.c_str(), vis_note.c_str());
        int i = 1;
        for (auto& ev : events) {
            std::printf("  %d. %-16s mag %5.2f  %s UT  closest approach to site: %6.1f km  "
                        "moon_alt=%6.1f  sun_alt=%6.1f  illum=%.2f\n",
                        i++, ev.star.name.c_str(), ev.star.vmag, jd_to_string(ev.circ.jd_tt).c_str(),
                        ev.dist_km, ev.circ.moon_alt_deg, ev.circ.sun_alt_deg, ev.circ.illum_frac);
        }

        if (!csv_out.empty()) {
            FILE* f = std::fopen(csv_out.c_str(), "w");
            if (f) {
                std::fprintf(f, "n,star,mag,time_utc,dist_km,moon_alt_deg,sun_alt_deg,illum_frac,star_alt_deg,star_az_deg\n");
                int n = 1;
                for (auto& ev : events) {
                    std::fprintf(f, "%d,%s,%.2f,%s,%.1f,%.1f,%.1f,%.3f,%.1f,%.1f\n",
                                 n++, ev.star.name.c_str(), ev.star.vmag, jd_to_string(ev.circ.jd_tt).c_str(),
                                 ev.dist_km, ev.circ.moon_alt_deg, ev.circ.sun_alt_deg, ev.circ.illum_frac,
                                 ev.circ.star_alt_deg, ev.circ.star_az_deg);
                }
                std::fclose(f);
                std::printf("\nCSV written to %s\n", csv_out.c_str());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
