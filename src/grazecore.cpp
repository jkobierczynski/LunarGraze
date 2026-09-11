// SPDX-License-Identifier: GPL-3.0-or-later
#include "grazecore.hpp"
#include "frames.hpp"
#include "time_utils.hpp"
#include <cmath>
#include <algorithm>
#include <functional>
#include <limits>
#include <cstdio>

namespace gg {

namespace {

constexpr double kKmPerDegLat = 111.32;
constexpr double kMoonRadiusKm = 1737.4;

// Geocentric close-approach coarse-scan gate (deg) and sample cadence (h) --
// same reasoning/values as graze_finder.py's COARSE_GATE_DEG/COARSE_STEP_HOURS:
// a real graze/occultation candidate can never exceed the Moon's angular
// radius (~0.24-0.29 deg) plus its horizontal parallax (~0.90-1.02 deg);
// 1.7 deg has healthy margin above that ceiling. At that gate the longest a
// star can stay under it is ~2*1.7/0.55 =~ 6.2 h (Moon's ~0.55 deg/h
// geocentric relative motion), so a 3 h sample cadence can never let a
// shallow approach fall entirely between two samples.
constexpr double kCoarseGateDeg = 1.7;
constexpr double kCoarseStepHours = 3.0;

struct SiteFrame {
    mg::Vec3 pos;   // km, ICRF-equatorial-equivalent (GMST-rotated ECEF)
    mg::Vec3 up, north, east; // unit vectors, same frame
};

SiteFrame site_frame_at(const Site& site, double jd_tt) {
    double jd_ut1 = mg::jd_tt_to_ut1(jd_tt);
    double gmst = mg::gmst_radians(jd_ut1);

    double lat = mg::deg2rad(site.lat_deg), lon = mg::deg2rad(site.lon_deg);
    double sl = std::sin(lat), cl = std::cos(lat), so = std::sin(lon), co = std::cos(lon);
    mg::Vec3 up_ecef{cl * co, cl * so, sl};
    mg::Vec3 east_ecef{-so, co, 0.0};
    mg::Vec3 north_ecef{-sl * co, -sl * so, cl};

    SiteFrame sf;
    sf.pos = mg::ecef_to_gcrs(mg::geodetic_to_ecef(site.lat_deg, site.lon_deg, site.elevation_m), gmst);
    sf.up = mg::ecef_to_gcrs(up_ecef, gmst);
    sf.north = mg::ecef_to_gcrs(north_ecef, gmst);
    sf.east = mg::ecef_to_gcrs(east_ecef, gmst);
    return sf;
}

void altaz_of_direction(const SiteFrame& sf, const mg::Vec3& unit_dir, double& alt_deg, double& az_deg) {
    double up_c = unit_dir.dot(sf.up);
    up_c = std::max(-1.0, std::min(1.0, up_c));
    alt_deg = mg::rad2deg(std::asin(up_c));
    double e = unit_dir.dot(sf.east), n = unit_dir.dot(sf.north);
    az_deg = mg::rad2deg(std::atan2(e, n));
    if (az_deg < 0) az_deg += 360.0;
}

double angle_between_deg(const mg::Vec3& a, const mg::Vec3& b) {
    double c = a.dot(b); // both are expected unit vectors
    c = std::max(-1.0, std::min(1.0, c));
    return mg::rad2deg(std::acos(c));
}

// Everything needed to evaluate margin (separation - Moon radius) at one
// instant, plus (cheaply, from the same intermediate values) Moon altitude
// -- this is what the golden-section search and coarse scan repeatedly call.
struct SepSample {
    double sep_deg;
    double moon_radius_deg;
    double moon_alt_deg;
};

SepSample separation_at(const Site& site, const mg::Star& star, const AreaEphemeris& eph, double jd_tt) {
    auto e = eph.at(jd_tt);
    SiteFrame sf = site_frame_at(site, jd_tt);

    mg::Vec3 moon_topo = e.moon - sf.pos;
    double moon_dist = moon_topo.norm();
    mg::Vec3 moon_dir = moon_topo.normalized();
    double moon_alt, moon_az;
    altaz_of_direction(sf, moon_dir, moon_alt, moon_az);

    mg::Vec3 star_geo = star.direction_at(jd_tt);
    mg::Vec3 star_dir = mg::Star::apply_aberration(star_geo, e.earth_vel);

    SepSample s;
    s.sep_deg = angle_between_deg(star_dir, moon_dir);
    s.moon_radius_deg = mg::rad2deg(std::asin(std::min(1.0, kMoonRadiusKm / moon_dist)));
    s.moon_alt_deg = moon_alt;
    return s;
}

// Full circumstances at one instant (sun altitude, illuminated fraction,
// star alt/az too) -- only needed once per refined event, not inside the
// hot search loops, so it's kept as a separate, slightly heavier function.
Circumstances full_circumstances(const Site& site, const mg::Star& star, const AreaEphemeris& eph, double jd_tt) {
    auto e = eph.at(jd_tt);
    SiteFrame sf = site_frame_at(site, jd_tt);

    mg::Vec3 moon_topo = e.moon - sf.pos;
    double moon_dist = moon_topo.norm();
    mg::Vec3 moon_dir = moon_topo.normalized();
    double moon_alt, moon_az;
    altaz_of_direction(sf, moon_dir, moon_alt, moon_az);

    mg::Vec3 sun_topo = e.sun - sf.pos;
    mg::Vec3 sun_dir = sun_topo.normalized();
    double sun_alt, sun_az;
    altaz_of_direction(sf, sun_dir, sun_alt, sun_az);

    mg::Vec3 star_geo = star.direction_at(jd_tt);
    mg::Vec3 star_dir = mg::Star::apply_aberration(star_geo, e.earth_vel);
    double star_alt, star_az;
    altaz_of_direction(sf, star_dir, star_alt, star_az);

    // Illuminated fraction: standard geocentric phase-angle formula from the
    // Sun-Earth distance R, Earth-Moon distance Delta, and their geocentric
    // elongation psi (matches pyephem's moon_phase to the level graze_finder
    // .py cross-validated against the printed Sterrengids 2026 almanac).
    double R = e.sun.norm(), Delta = e.moon.norm();
    double cos_psi = e.sun.normalized().dot(e.moon.normalized());
    cos_psi = std::max(-1.0, std::min(1.0, cos_psi));
    double psi = std::acos(cos_psi);
    double phase_angle = std::atan2(R * std::sin(psi), Delta - R * std::cos(psi));
    double illum = (1.0 + std::cos(phase_angle)) / 2.0;

    Circumstances c;
    c.jd_tt = jd_tt;
    c.sep_deg = angle_between_deg(star_dir, moon_dir);
    c.moon_radius_deg = mg::rad2deg(std::asin(std::min(1.0, kMoonRadiusKm / moon_dist)));
    c.margin_deg = c.sep_deg - c.moon_radius_deg;
    c.moon_alt_deg = moon_alt;
    c.sun_alt_deg = sun_alt;
    c.illum_frac = illum;
    c.star_alt_deg = star_alt;
    c.star_az_deg = star_az;
    return c;
}

constexpr double kGolden = 0.6180339887498949; // (sqrt(5)-1)/2

// Golden-section search for the instant of minimum star-Moon separation
// near center_jd, within +-half_window_min minutes, converging to
// tol_seconds. Direct port of graze_finder.py's local_minimum() -- see that
// function's docstring for why golden section (vs. a brute-force scan) is
// what keeps a whole-year run fast.
struct LocalMinResult { double jd_tt; SepSample sample; };

LocalMinResult local_minimum(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                              double center_jd, double half_window_min, double tol_seconds) {
    double half_window_days = half_window_min / 1440.0;
    double tol_days = std::max(tol_seconds, 1.0) / 86400.0;

    double a = center_jd - half_window_days;
    double b = center_jd + half_window_days;
    auto sep_at = [&](double t) { return separation_at(site, star, eph, t).sep_deg; };

    double c = b - kGolden * (b - a);
    double d = a + kGolden * (b - a);
    double fc = sep_at(c), fd = sep_at(d);
    while ((b - a) > tol_days) {
        if (fc < fd) {
            b = d; d = c; fd = fc;
            c = b - kGolden * (b - a);
            fc = sep_at(c);
        } else {
            a = c; c = d; fc = fd;
            d = a + kGolden * (b - a);
            fd = sep_at(d);
        }
    }
    double tmin = (a + b) / 2.0;
    LocalMinResult r;
    r.jd_tt = tmin;
    r.sample = separation_at(site, star, eph, tmin);
    return r;
}

// (margin, time) at a small window seeded from a nearby good time guess --
// what makes line-tracing fast (a few hundred evaluations per point, not a
// wide blind sweep). Mirrors graze_finder.py's _margin_at().
struct MarginResult { double margin_deg, jd_tt; };

MarginResult margin_at(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                        double jd_guess, double half_window_min = 9.0, double tol_seconds = 9.0) {
    auto r = local_minimum(site, star, eph, jd_guess, half_window_min, tol_seconds);
    MarginResult m;
    m.margin_deg = r.sample.sep_deg - r.sample.moon_radius_deg;
    m.jd_tt = r.jd_tt;
    return m;
}

// Standard Brent root-finding (Brent 1973 "Algorithms for Minimization
// without Derivatives", the inverse-quadratic-interpolation-with-bisection-
// fallback method also used by scipy.optimize.brentq / GSL's gsl_root_fsolver
// _brent) -- self-contained implementation, no external numerics dependency.
// f(lo) and f(hi) must have opposite signs. Throws std::runtime_error if not
// converged within max_iter (bracket_and_root below treats that the same as
// "no root found", matching graze_finder.py's ValueError handling).
double brent_root(const std::function<double(double)>& f, double lo, double hi, double xtol,
                   int max_iter = 100) {
    double a = lo, b = hi, fa = f(a), fb = f(b);
    if ((fa > 0.0 && fb > 0.0) || (fa < 0.0 && fb < 0.0)) {
#ifdef GRAZECORE_DEBUG_MARCH
        std::fprintf(stderr, "      brent init: a=%.6f fa=%+.6f  b=%.6f fb=%+.6f\n", a, fa, b, fb);
#endif
        throw std::runtime_error("brent_root: root not bracketed");
    }
    double c = b, fc = fb, d = b - a, e = b - a;
    const double eps = std::numeric_limits<double>::epsilon();
    for (int iter = 0; iter < max_iter; iter++) {
        if ((fb > 0.0 && fc > 0.0) || (fb < 0.0 && fc < 0.0)) {
            c = a; fc = fa; d = e = b - a;
        }
        if (std::fabs(fc) < std::fabs(fb)) {
            a = b; b = c; c = a;
            fa = fb; fb = fc; fc = fa;
        }
        double tol1 = 2.0 * eps * std::fabs(b) + 0.5 * xtol;
        double xm = 0.5 * (c - b);
        if (std::fabs(xm) <= tol1 || fb == 0.0) return b;
        if (std::fabs(e) >= tol1 && std::fabs(fa) > std::fabs(fb)) {
            double p, q, r, s = fb / fa;
            if (a == c) {
                p = 2.0 * xm * s;
                q = 1.0 - s;
            } else {
                q = fa / fc; r = fb / fc;
                p = s * (2.0 * xm * q * (q - r) - (b - a) * (r - 1.0));
                q = (q - 1.0) * (r - 1.0) * (s - 1.0);
            }
            if (p > 0.0) q = -q;
            p = std::fabs(p);
            double min1 = 3.0 * xm * q - std::fabs(tol1 * q);
            double min2 = std::fabs(e * q);
            if (2.0 * p < std::min(min1, min2)) { e = d; d = p / q; }
            else { d = xm; e = d; }
        } else {
            d = xm; e = d;
        }
        a = b; fa = fb;
        if (std::fabs(d) > tol1) b += d;
        else b += (xm > 0.0 ? tol1 : -tol1);
        fb = f(b);
    }
    throw std::runtime_error("brent_root: did not converge");
}

// A near-tangent graze can have an extremely narrow negative-margin band
// in latitude (observed: under ~0.1 deg for one real event cross-checked
// against the Python tool during development) -- narrow enough that the
// geometrically-growing step march below can land samples on both sides
// of it without either one catching the sign flip, because small
// (sub-arcsecond to few-arcsecond scale) differences between this engine's
// deliberately simplified geometry (see frames.hpp: GMST-only Earth
// rotation, no precession/nutation -- "mostly cancels" for a relative
// separation, but not exactly) and a fully rigorous ephemeris can leave
// the margin at a sampled point sitting on the "wrong" side of zero by a
// few arcseconds right at the edge of such a band. Guard against this: if
// two consecutive same-sign samples both have a small margin (within
// kNearZeroThresholdDeg), bisect between them looking for a hidden sign
// flip before concluding there isn't one.
constexpr double kNearZeroThresholdDeg = 0.02; // ~72 arcsec

bool find_hidden_crossing(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                           double lon_deg, double win,
                           double lat_a, double m_a, double t_a,
                           double lat_b, double m_b, double t_b,
                           double& out_lat_lo, double& out_m_lo, double& out_t_lo,
                           double& out_lat_hi, double& out_m_hi, double& out_t_hi,
                           int depth = 4) {
    if ((m_a > 0) != (m_b > 0)) {
        out_lat_lo = lat_a; out_m_lo = m_a; out_t_lo = t_a;
        out_lat_hi = lat_b; out_m_hi = m_b; out_t_hi = t_b;
        return true;
    }
    if (depth <= 0) return false;
    if (std::fabs(m_a) > kNearZeroThresholdDeg && std::fabs(m_b) > kNearZeroThresholdDeg) return false;

    double lat_mid = 0.5 * (lat_a + lat_b);
    Site sm = site; sm.lon_deg = lon_deg; sm.lat_deg = lat_mid;
    MarginResult mr = margin_at(sm, star, eph, t_a, win, 9.0);
    double m_mid = mr.margin_deg, t_mid = mr.jd_tt;

    if (find_hidden_crossing(site, star, eph, lon_deg, win, lat_a, m_a, t_a, lat_mid, m_mid, t_mid,
                              out_lat_lo, out_m_lo, out_t_lo, out_lat_hi, out_m_hi, out_t_hi, depth - 1))
        return true;
    return find_hidden_crossing(site, star, eph, lon_deg, win, lat_mid, m_mid, t_mid, lat_b, m_b, t_b,
                                 out_lat_lo, out_m_lo, out_t_lo, out_lat_hi, out_m_hi, out_t_hi, depth - 1);
}

// March in latitude from (lat_start, margin_start) until the margin
// (separation - Moon radius) changes sign, then Brent-root to the crossing.
// Direct port of graze_finder.py's _bracket_and_root() -- see that
// function's long docstring for why the step starts small and grows
// geometrically, and why a "root" is only accepted after a sanity re-check
// (protects against the line-2/Maia spurious-crossing bug found and fixed
// in the Python tool: a big latitude jump can carry the true closest-
// approach time outside margin_at's search window, making golden section
// lock onto the wrong local minimum and brentq "converge" to a confidently
// wrong root).
struct BracketResult { bool ok; double lat_deg, jd_tt, margin_deg; };

BracketResult bracket_and_root(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                                double lon_deg, double lat_start, double margin_start, double jd_start,
                                int direction, double max_deg = 15.0, double step_deg = 1.0,
                                double xtol_deg = 2e-4, double sanity_arcsec = 8.0) {
    Site s = site; s.lon_deg = lon_deg;
    // margin_start isn't used directly -- every comparison below re-evaluates
    // the previous point's margin with the current iteration's own window
    // first (see the note at the top of the loop), so the caller-supplied
    // starting margin only ever mattered as a value to discard anyway.
    (void)margin_start;
    double prev_lat = lat_start, prev_t = jd_start;
    double lat = lat_start;
    double step = std::min(step_deg, 0.1);
    double traveled = 0.0;

    while (traveled < max_deg - 1e-9) {
        step = std::min(step, max_deg - traveled);
        lat = lat + direction * step;
        traveled += step;
        double win = std::max(9.0, std::min(45.0, std::round(step * 40.0)));
        s.lat_deg = lat;
        MarginResult mr = margin_at(s, star, eph, prev_t, win, 9.0);
        double m = mr.margin_deg, t = mr.jd_tt;
#ifdef GRAZECORE_DEBUG_MARCH
        std::fprintf(stderr, "  march: traveled=%.4f step=%.4f lat=%.4f win=%.1f m=%+.6f t=%.6f\n",
                     traveled, step, lat, win, m, t);
#endif

        // Re-evaluate the PREVIOUS point's margin with THIS iteration's
        // window before comparing signs. margin_at's result is genuinely
        // window-size-dependent for a shallow/near-tangent approach (a
        // narrow window can converge to a shallow, boundary-pinned pseudo-
        // minimum well short of the true one -- see grazecore.cpp's design
        // notes above find_hidden_crossing); prev_m was computed with
        // whatever (often smaller) window was in effect *last* iteration
        // (or, for the very first iteration, with refine_event's own
        // 4-minute final-stage window, smaller still). Comparing it
        // directly against a value computed with a different window can
        // manufacture or hide a sign change that doesn't reflect the same
        // underlying minimum -- and even worse, can make a bracket that
        // *looked* valid here silently fail brent_root's own re-evaluation
        // of the same endpoint with the same (correct) window, since that
        // recomputation won't agree with the stale prev_m. Keeping both
        // endpoints on the same window before any comparison removes that
        // whole failure mode.
        Site sp = site; sp.lon_deg = lon_deg; sp.lat_deg = prev_lat;
        MarginResult mr_prev = margin_at(sp, star, eph, prev_t, win, 9.0);
        double prev_m_fresh = mr_prev.margin_deg, prev_t_fresh = mr_prev.jd_tt;

        double lat_lo_c, m_lo_c, t_lo_c, lat_hi_c, m_hi_c, t_hi_c;
        bool crossing = find_hidden_crossing(site, star, eph, lon_deg, win,
                                              prev_lat, prev_m_fresh, prev_t_fresh, lat, m, t,
                                              lat_lo_c, m_lo_c, t_lo_c, lat_hi_c, m_hi_c, t_hi_c);
        if (crossing) {
            double lo = std::min(lat_lo_c, lat_hi_c), hi = std::max(lat_lo_c, lat_hi_c);
            double t_lo = (lat_lo_c < lat_hi_c) ? t_lo_c : t_hi_c;
            double t_hi = (lat_lo_c < lat_hi_c) ? t_hi_c : t_lo_c;
            double seed_lo = t_lo, seed_hi = t_hi;

            auto f = [&](double la) -> double {
                double seed = (std::fabs(la - lo) <= std::fabs(la - hi)) ? seed_lo : seed_hi;
                Site s2 = site; s2.lon_deg = lon_deg; s2.lat_deg = la;
                MarginResult r = margin_at(s2, star, eph, seed, win, 9.0);
                if (std::fabs(la - lo) <= std::fabs(la - hi)) seed_lo = r.jd_tt;
                else seed_hi = r.jd_tt;
                return r.margin_deg;
            };

            try {
                double root = brent_root(f, lo, hi, xtol_deg);
                double seed = (std::fabs(root - lo) <= std::fabs(root - hi)) ? seed_lo : seed_hi;
                Site s3 = site; s3.lon_deg = lon_deg; s3.lat_deg = root;
                MarginResult final_r = margin_at(s3, star, eph, seed, win, 9.0);
#ifdef GRAZECORE_DEBUG_MARCH
                std::fprintf(stderr, "    sign-change: lo=%.4f hi=%.4f -> root=%.5f final_margin=%.6f (%.2f arcsec)\n",
                             lo, hi, root, final_r.margin_deg, final_r.margin_deg * 3600.0);
#endif
                if (std::fabs(final_r.margin_deg) * 3600.0 <= sanity_arcsec)
                    return {true, root, final_r.jd_tt, final_r.margin_deg};
                // spurious sign change -- keep marching rather than return it
            } catch (const std::runtime_error& e) {
#ifdef GRAZECORE_DEBUG_MARCH
                std::fprintf(stderr, "    sign-change: lo=%.4f hi=%.4f -> brent threw: %s\n", lo, hi, e.what());
#endif
                // bracket didn't hold on re-evaluation either; keep marching
            }
        }
        prev_lat = lat; prev_t = t;
        step *= 1.7;
    }
    return {false, 0, 0, 0};
}

// Trace the graze line across a spread of longitudes centered on the site.
// Direct port of graze_finder.py's trace_graze_line().
std::vector<LinePoint> trace_graze_line(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                                         double site_margin_deg, double site_jd,
                                         double lon_half_span_deg, int n_points = 17) {
    auto anchor = bracket_and_root(site, star, eph, site.lon_deg, site.lat_deg, site_margin_deg, site_jd,
                                    +1, 15.0, 1.5);
    if (!anchor.ok)
        anchor = bracket_and_root(site, star, eph, site.lon_deg, site.lat_deg, site_margin_deg, site_jd,
                                   -1, 15.0, 1.5);
    if (!anchor.ok) return {};

    double lat_a = anchor.lat_deg, t_a = anchor.jd_tt, m_a = anchor.margin_deg;

    Site sa = site; sa.lat_deg = lat_a + 0.05;
    MarginResult mr_plus = margin_at(sa, star, eph, t_a);
    double grad = (mr_plus.margin_deg - m_a) / 0.05; // deg margin per deg latitude

    double lon_step = 2 * lon_half_span_deg / (n_points - 1);
    int n_side = n_points / 2;

    auto march = [&](int sign) {
        std::vector<LinePoint> pts;
        double lat_prev = lat_a, t_prev = t_a, lon_prev = site.lon_deg, g = grad;
        for (int k = 0; k < n_side; k++) {
            double lon_next = lon_prev + sign * lon_step;
            Site s0 = site; s0.lat_deg = lat_prev; s0.lon_deg = lon_next;
            MarginResult r0 = margin_at(s0, star, eph, t_prev);
            double m0 = r0.margin_deg, t0 = r0.jd_tt;

            bool have_res = false;
            double res_lat = 0, res_t = 0, res_m = 0;

            if (std::fabs(g) > 1e-6) {
                double lat_guess = lat_prev - m0 / g;
                Site s1 = site; s1.lat_deg = lat_guess; s1.lon_deg = lon_next;
                MarginResult r1 = margin_at(s1, star, eph, t0);
                double m1 = r1.margin_deg, t1 = r1.jd_tt;
                if ((m1 > 0) != (m0 > 0)) {
                    double lo = std::min(lat_prev, lat_guess), hi = std::max(lat_prev, lat_guess);
                    double seed_lo = (lat_prev < lat_guess) ? t0 : t1;
                    double seed_hi = (lat_prev < lat_guess) ? t1 : t0;
                    auto f = [&](double la) -> double {
                        double seed = (std::fabs(la - lo) <= std::fabs(la - hi)) ? seed_lo : seed_hi;
                        Site s2 = site; s2.lat_deg = la; s2.lon_deg = lon_next;
                        MarginResult r = margin_at(s2, star, eph, seed);
                        return r.margin_deg;
                    };
                    try {
                        double root = brent_root(f, lo, hi, 2e-4);
                        double seed = (std::fabs(root - lo) <= std::fabs(root - hi)) ? seed_lo : seed_hi;
                        Site s3 = site; s3.lat_deg = root; s3.lon_deg = lon_next;
                        MarginResult final_r = margin_at(s3, star, eph, seed);
                        if (std::fabs(final_r.margin_deg) * 3600.0 <= 8.0) {
                            have_res = true;
                            res_lat = root; res_t = final_r.jd_tt; res_m = final_r.margin_deg;
                        }
                    } catch (const std::runtime_error&) {
                        // fall through to bracket search below
                    }
                }
            }
            if (!have_res) {
                auto br = bracket_and_root(site, star, eph, lon_next, lat_prev, m0, t0, +1, 3.0, 0.25);
                if (!br.ok)
                    br = bracket_and_root(site, star, eph, lon_next, lat_prev, m0, t0, -1, 3.0, 0.25);
                if (!br.ok) break;
                have_res = true; res_lat = br.lat_deg; res_t = br.jd_tt; res_m = br.margin_deg;
            }

            lat_prev = res_lat; t_prev = res_t;
            Site sg = site; sg.lat_deg = lat_prev + 0.05; sg.lon_deg = lon_next;
            MarginResult mg2 = margin_at(sg, star, eph, t_prev);
            double new_g = (mg2.margin_deg - res_m) / 0.05;
            if (std::fabs(new_g) > 1e-6) g = new_g;

            pts.push_back({lon_next, lat_prev, t_prev, res_m * 3600.0});
            lon_prev = lon_next;
        }
        return pts;
    };

    auto west = march(-1);
    std::reverse(west.begin(), west.end());
    auto east = march(+1);

    std::vector<LinePoint> out;
    out.reserve(west.size() + east.size() + 1);
    out.insert(out.end(), west.begin(), west.end());
    out.push_back({site.lon_deg, lat_a, t_a, m_a * 3600.0});
    out.insert(out.end(), east.begin(), east.end());
    return out;
}

double point_to_polyline_km(double site_lat, double site_lon, const std::vector<LinePoint>& pts) {
    auto to_xy = [&](double la, double lo) {
        return std::pair<double, double>{lo * km_per_deg_lon(site_lat), la * kKmPerDegLat};
    };
    auto [px, py] = to_xy(site_lat, site_lon);
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i + 1 < pts.size(); i++) {
        auto [x1, y1] = to_xy(pts[i].lat_deg, pts[i].lon_deg);
        auto [x2, y2] = to_xy(pts[i + 1].lat_deg, pts[i + 1].lon_deg);
        double dx = x2 - x1, dy = y2 - y1;
        double seglen2 = dx * dx + dy * dy;
        double t = 0.0;
        if (seglen2 >= 1e-9) t = std::max(0.0, std::min(1.0, ((px - x1) * dx + (py - y1) * dy) / seglen2));
        double cx = x1 + t * dx, cy = y1 + t * dy;
        double d = std::hypot(px - cx, py - cy);
        best = std::min(best, d);
    }
    return best;
}

} // namespace

double km_per_deg_lon(double lat_deg) { return kKmPerDegLat * std::cos(mg::deg2rad(lat_deg)); }

double debug_topocentric_separation_deg(const Site& site, const mg::Star& star,
                                         const AreaEphemeris& eph, double jd_tt,
                                         double* moon_alt_deg_out) {
    SepSample s = separation_at(site, star, eph, jd_tt);
    if (moon_alt_deg_out) *moon_alt_deg_out = s.moon_alt_deg;
    return s.sep_deg;
}

double debug_margin_at(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                        double jd_guess, double half_window_min, double* jd_out) {
    MarginResult m = margin_at(site, star, eph, jd_guess, half_window_min, 9.0);
    if (jd_out) *jd_out = m.jd_tt;
    return m.margin_deg;
}

DebugBracketResult debug_bracket_and_root(const Site& site, const mg::Star& star, const AreaEphemeris& eph,
                                           double lon_deg, double lat_start, double margin_start,
                                           double jd_start, int direction, double max_deg, double step_deg) {
    BracketResult r = bracket_and_root(site, star, eph, lon_deg, lat_start, margin_start, jd_start,
                                        direction, max_deg, step_deg);
    return {r.ok, r.lat_deg, r.jd_tt, r.margin_deg};
}

std::vector<GrazeEvent> find_grazes(
    const Site& site, const AreaEphemeris& eph, const std::vector<mg::Star>& stars,
    double jd_start, double jd_end, const Filters& filt,
    std::function<bool(int, int)> progress_cb,
    std::function<void(const std::string&)> verbose_cb) {

    double prescreen_deg = std::min(4.0, std::max(0.5, 3.0 * filt.radius_km / 6371.0));

    std::vector<GrazeEvent> events;
    int done = 0;
    for (const auto& star : stars) {
        // --- coarse scan for geocentric(ish, topocentric-at-site) close approaches ---
        std::vector<std::pair<double, double>> windows; // (t0, t1)
        {
            double step = kCoarseStepHours / 24.0;
            double t = jd_start;
            std::vector<double> hits;
            while (t < jd_end) {
                if (t >= eph.jd_min() && t <= eph.jd_max()) {
                    SepSample s = separation_at(site, star, eph, t);
                    if (s.sep_deg < kCoarseGateDeg) hits.push_back(t);
                }
                t += step;
            }
            for (double h : hits) {
                if (!windows.empty() && (h - windows.back().second) < 2 * kCoarseStepHours / 24.0 + 0.01)
                    windows.back().second = h;
                else
                    windows.push_back({h, h});
            }
        }

        for (auto& w : windows) {
            // A local search window can in principle reach just past the
            // baked ephemeris table's covered range (e.g. a candidate right
            // at the start/end of the requested date range, or a wide
            // latitude-marching step during line tracing) -- AreaEphemeris::
            // at() throws rather than silently extrapolating a lunar
            // position, by design. Skip that one window/point rather than
            // aborting the whole search; bake the table with a few days'
            // padding beyond your intended --start/--end to avoid this
            // happening for genuine candidates near the edges.
            try {
                double t0 = w.first;
                auto refined = local_minimum(site, star, eph, t0, 90.0, 20.0);
                refined = local_minimum(site, star, eph, refined.jd_tt, 4.0, 1.0);
                Circumstances circ = full_circumstances(site, star, eph, refined.jd_tt);

                if (std::fabs(circ.margin_deg) > prescreen_deg) continue;
                if (filt.filter_min_moon_alt && circ.moon_alt_deg < filt.min_moon_alt_deg) continue;
                if (filt.filter_max_sun_alt && circ.sun_alt_deg > filt.max_sun_alt_deg) continue;
                if (filt.filter_min_illum && circ.illum_frac < filt.min_illum) continue;
                if (filt.filter_max_illum && circ.illum_frac > filt.max_illum) continue;

                double lon_span = std::max(1.0, std::min(6.0, (filt.radius_km * 2.2) / km_per_deg_lon(site.lat_deg)));
                auto line = trace_graze_line(site, star, eph, circ.margin_deg, circ.jd_tt, lon_span);
                if (line.empty()) {
                    if (verbose_cb)
                        verbose_cb("[" + star.name + "] no line anchor near jd=" +
                                   std::to_string(circ.jd_tt) + " margin_deg=" + std::to_string(circ.margin_deg));
                    continue;
                }
                double dist = point_to_polyline_km(site.lat_deg, site.lon_deg, line);
                if (dist <= filt.radius_km) {
                    GrazeEvent ev;
                    ev.star = star; ev.circ = circ; ev.line = std::move(line); ev.dist_km = dist;
                    events.push_back(std::move(ev));
                }
            } catch (const std::exception& e) {
                if (verbose_cb)
                    verbose_cb(std::string("[") + star.name + "] skipped a window: " + e.what());
            }
        }

        done++;
        if (progress_cb && !progress_cb(done, static_cast<int>(stars.size())))
            return events; // cancelled
    }

    std::sort(events.begin(), events.end(),
              [](const GrazeEvent& a, const GrazeEvent& b) { return a.circ.jd_tt < b.circ.jd_tt; });
    return events;
}

} // namespace gg
