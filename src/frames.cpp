// SPDX-License-Identifier: GPL-3.0-or-later
// Bundled, unmodified copy of ../../src/frames.cpp from the main moon-graze
// C++ engine, so this directory builds standalone. Keep in sync by hand if
// the original changes.
#include "frames.hpp"
#include <cmath>

namespace mg {

Vec3 geodetic_to_ecef(double lat_deg, double lon_deg, double height_m) {
    // WGS84
    const double a = 6378.137;             // km
    const double f = 1.0 / 298.257223563;
    const double e2 = f * (2 - f);

    double lat = deg2rad(lat_deg);
    double lon = deg2rad(lon_deg);
    double h = height_m / 1000.0;          // km

    double sinlat = std::sin(lat), coslat = std::cos(lat);
    double N = a / std::sqrt(1.0 - e2 * sinlat * sinlat);

    double x = (N + h) * coslat * std::cos(lon);
    double y = (N + h) * coslat * std::sin(lon);
    double z = (N * (1.0 - e2) + h) * sinlat;
    return {x, y, z};
}

Vec3 ecef_to_gcrs(const Vec3& ecef, double gmst_rad) {
    // Earth-fixed -> quasi-inertial: rotate by +GMST about Z.
    return Mat3::R3(-gmst_rad).apply(ecef);
}

Mat3 icrs_to_selenographic(double phi, double theta, double psi) {
    // Taylor, Kubitschek & Steiner (2011) write the ICRS->PA rotation as
    // r2 = R3(-phi) R1(-theta) R3(-psi) r1 for "the" Euler angles obtained
    // by interrogating a JPL lunar ephemeris. Empirically, for the specific
    // (phi,theta,psi) triple returned by jplephem's Ephemeris.position
    // ('librations', ...) against DE421, that formula places the sub-Earth
    // point tens of degrees away from selenographic (0,0) -- physically
    // impossible (libration is bounded to a few degrees). Its *transpose*
    // (equivalently R3(psi) R1(theta) R3(phi), no negation) places the
    // sub-Earth point correctly within the expected +-8 deg/+-7 deg
    // libration bounds, i.e. jplephem's Euler-angle triple is the PA->ICRS
    // rotation relative to Taylor's stated convention, not ICRS->PA.
    // Verified numerically (see git history / VALIDATION.md) before
    // shipping this rather than trusting the cited formula blindly.
    return (Mat3::R3(-phi) * Mat3::R1(-theta) * Mat3::R3(-psi)).transposed();
}

} // namespace mg
