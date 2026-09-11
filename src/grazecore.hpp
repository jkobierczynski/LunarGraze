// SPDX-License-Identifier: GPL-3.0-or-later
// LunarGraze -- C++ port of tools/graze_finder/graze_finder.py's algorithm:
// find and trace grazing-lunar-occultation lines near a site, for any star
// magnitude limit and date range. See that script's module docstring for
// the full method description and caveats (spherical-Moon geometry, no DEM
// limb topography, no atmospheric refraction) -- unchanged here.
#pragma once
#include "vec3.hpp"
#include "star.hpp"
#include "areaephem.hpp"
#include <vector>
#include <string>
#include <functional>
#include <atomic>

namespace gg {

struct Site {
    double lat_deg = 0, lon_deg = 0, elevation_m = 100;
};

// Circumstances of one star-Moon closest approach at a fixed site.
struct Circumstances {
    double jd_tt = 0;
    double sep_deg = 0;          // topocentric star-to-Moon-centre separation
    double moon_radius_deg = 0;  // Moon's topocentric angular radius
    double margin_deg = 0;       // sep_deg - moon_radius_deg (<0 means an occultation, not just a graze)
    double moon_alt_deg = 0;
    double sun_alt_deg = 0;
    double illum_frac = 0;       // 0 = new Moon, 1 = full Moon
    double star_alt_deg = 0;
    double star_az_deg = 0;
};

struct LinePoint {
    double lon_deg, lat_deg, jd_tt, margin_arcsec;
};

struct GrazeEvent {
    mg::Star star;
    Circumstances circ;
    std::vector<LinePoint> line; // west to east
    double dist_km = 0;          // closest approach of the line to the site
};

struct Filters {
    double mag_limit = 4.0;
    double radius_km = 100.0;

    bool filter_min_moon_alt = false;
    double min_moon_alt_deg = 0.0;

    bool filter_max_sun_alt = false;
    double max_sun_alt_deg = 0.0;

    bool filter_min_illum = false;
    double min_illum = 0.0;

    bool filter_max_illum = false;
    double max_illum = 1.0;
};

double km_per_deg_lon(double lat_deg);

// Exposed for the test harness / debugging only: topocentric star-Moon
// separation (deg) at one instant, plus the Moon's altitude at the site.
double debug_topocentric_separation_deg(const Site& site, const mg::Star& star,
                                         const AreaEphemeris& eph, double jd_tt,
                                         double* moon_alt_deg_out = nullptr);

// Exposed for the test harness / debugging only: margin (separation minus
// Moon's angular radius) at the time of closest approach found by golden-
// section search seeded near jd_guess -- i.e. what the internal line-
// tracing march actually evaluates at each latitude (unlike the function
// above, which uses a single fixed instant).
double debug_margin_at(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                        double jd_guess, double half_window_min, double* jd_out = nullptr);

// Exposed for the test harness / debugging only: direct call into
// bracket_and_root(), mirroring graze_finder.py's _bracket_and_root().
struct DebugBracketResult { bool ok; double lat_deg, jd_tt, margin_deg; };
DebugBracketResult debug_bracket_and_root(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                                           double lon_deg, double lat_start, double margin_start,
                                           double jd_start, int direction, double max_deg, double step_deg);

// progress_cb(stars_done, stars_total) is called after each candidate star
// finishes; returning false requests cancellation (checked promptly).
// verbose_cb, if set, receives human-readable progress/log lines.
std::vector<GrazeEvent> find_grazes(
    const Site& site, const AreaEphemeris& eph, const std::vector<mg::Star>& stars,
    double jd_start, double jd_end, const Filters& filt,
    std::function<bool(int, int)> progress_cb = {},
    std::function<void(const std::string&)> verbose_cb = {});

} // namespace gg
