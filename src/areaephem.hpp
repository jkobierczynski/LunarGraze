// SPDX-License-Identifier: GPL-3.0-or-later
// LunarGraze -- loads the baked Moon+Sun ephemeris table produced by
// gen_area_ephemeris.py. Same 4-point-Lagrange interpolation scheme as
// ../../include/ephemeris.hpp (mg::Ephemeris), but reading the 10-column
// (Sun-included) format that tool writes -- see gen_area_ephemeris.py's
// header for why this is a separate loader/format rather than reusing
// mg::Ephemeris directly (that one is Sun-less, tuned for the original
// single-known-event flash-timing engine).
#pragma once
#include "vec3.hpp"
#include <string>
#include <vector>
#include <stdexcept>

namespace gg {

struct AreaEphemerisSample {
    double jd_tt;
    mg::Vec3 moon;       // geocentric Moon position, ICRF equatorial J2000, km
    mg::Vec3 sun;        // geocentric Sun position, ICRF equatorial J2000, km
    mg::Vec3 earth_vel;  // Earth SSB velocity, km/s (stellar aberration)
};

class AreaEphemeris {
public:
    explicit AreaEphemeris(const std::string& path);

    AreaEphemerisSample at(double jd_tt) const;

    double jd_min() const { return rows_.front().jd_tt; }
    double jd_max() const { return rows_.back().jd_tt; }

private:
    std::vector<AreaEphemerisSample> rows_;
};

} // namespace gg
