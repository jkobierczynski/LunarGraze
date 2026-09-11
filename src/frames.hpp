// SPDX-License-Identifier: GPL-3.0-or-later
// Bundled, unmodified copy of ../../include/frames.hpp from the main
// moon-graze C++ engine, so this directory builds standalone. Keep in sync
// by hand if the original changes.
// moon-graze -- reference frame transformations.
//
// KNOWN SIMPLIFICATIONS (documented, not hidden -- see README.md):
//  * Earth-fixed -> ICRF uses Greenwich Mean Sidereal Time only: no
//    precession, nutation, or polar motion. Precession/nutation reorient the
//    equatorial frame itself, not the star-vs-Moon relative geometry (which
//    is what determines occultation), so as long as everything is expressed
//    consistently in the ICRF/GCRS frame this mostly cancels out for the sky
//    geometry; the residual effect on where the answer lands on the *Earth's
//    surface* is at the polar-motion/frame-bias level, a few metres.
//    Skipping UT1-UTC (~<=0.9s) leaves a few-hundred-metre longitude error.
//  * WGS84 ellipsoid, no local geoid/height-above-ellipsoid subtleties.
#pragma once
#include "vec3.hpp"

namespace mg {

// Observer geodetic position -> Earth-fixed (ECEF) Cartesian, km, WGS84.
Vec3 geodetic_to_ecef(double lat_deg, double lon_deg, double height_m);

// Rotate an Earth-fixed (ECEF) vector into the ICRF/GCRS-equivalent frame
// used by the ephemeris, using Greenwich Mean Sidereal Time as the (only)
// rotation -- see the simplifications note above.
Vec3 ecef_to_gcrs(const Vec3& ecef, double gmst_rad);

// Build the ICRS -> lunar principal-axis (selenographic body-fixed) rotation
// matrix from the DE421 libration Euler angles (phi, theta, psi), following
// Taylor, Kubitschek & Steiner (USNO/HMNAO 2011): r_pa = R3(-phi) R1(-theta) R3(-psi) r_icrs.
Mat3 icrs_to_selenographic(double phi, double theta, double psi);

} // namespace mg
