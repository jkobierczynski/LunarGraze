// SPDX-License-Identifier: GPL-3.0-or-later
// Bundled, unmodified copy of ../../include/star.hpp from the main
// moon-graze C++ engine, so this directory builds standalone. Keep in sync
// by hand if the original changes.
// moon-graze -- star position with linear proper-motion propagation and
// classical (first-order, v/c) annual aberration.
//
// KNOWN SIMPLIFICATIONS (documented -- see README.md):
//  * Parallax is not modelled: for essentially every real occultation target
//    (a background catalogue star, not a nearby dwarf) annual parallax is
//    far below the arcsecond level and utterly negligible next to the
//    mean-limb model's own km-scale simplification.
//  * Gravitational light deflection by the Sun (up to ~1.75" AT the solar
//    limb, a few mas typical) is not applied.
//  * Proper motion is propagated linearly in a tangent-plane sense, fine
//    over the few-decade spans occultation predictions are used for.
#pragma once
#include "vec3.hpp"
#include <string>

namespace mg {

struct Star {
    std::string name;
    double ra_deg;         // J2000 ICRS right ascension
    double dec_deg;        // J2000 ICRS declination
    double pmra_masyr = 0; // proper motion in RA, mas/yr (mu_alpha* , already includes cos(dec))
    double pmdec_masyr = 0;// proper motion in Dec, mas/yr
    double vmag = 0;

    // J2000.0 = JD 2451545.0
    static constexpr double kJ2000 = 2451545.0;

    // Geometric (unaberrated) direction unit vector at the given TT Julian date.
    Vec3 direction_at(double jd_tt) const {
        double years = (jd_tt - kJ2000) / 365.25;
        double dec = dec_deg + (pmdec_masyr / 3.6e6) * years; // mas -> deg
        double cosdec0 = std::cos(deg2rad(dec_deg));
        double ra = ra_deg;
        if (std::fabs(cosdec0) > 1e-9)
            ra = ra_deg + (pmra_masyr / 3.6e6) * years / cosdec0;
        double a = deg2rad(ra), d = deg2rad(dec);
        return Vec3(std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d));
    }

    // Apply classical first-order annual aberration given the Earth's SSB
    // velocity (km/s). s_hat must already be a unit vector.
    static Vec3 apply_aberration(const Vec3& s_hat, const Vec3& earth_vel_kms) {
        const double c_km_s = 299792.458;
        Vec3 beta = earth_vel_kms * (1.0 / c_km_s);
        Vec3 apparent = s_hat + beta - s_hat * s_hat.dot(beta);
        return apparent.normalized();
    }
};

} // namespace mg
