// SPDX-License-Identifier: GPL-3.0-or-later
// LunarGraze -- HYG star catalogue loader (same bundled hygdata_v41.csv as
// tools/graze_finder/graze_finder.py; astronexus/HYG-Database on GitHub,
// itself built from Hipparcos/Yale Bright Star/Gliese). Keeps every star at
// or brighter than a magnitude limit whose ecliptic latitude is within
// ECLIPTIC_LAT_CUTOFF_DEG of 0 -- the only stars the Moon's 5.145-degree-
// inclined, 18.6-year-precessing orbit can ever occult.
#pragma once
#include "star.hpp"
#include <string>
#include <vector>

namespace gg {

constexpr double kEclipticLatCutoffDeg = 8.0;

// Load HYG rows at or brighter than mag_limit, filtered to
// |ecliptic latitude| < kEclipticLatCutoffDeg. Throws on unreadable file.
std::vector<mg::Star> load_hyg_catalog(const std::string& path, double mag_limit);

} // namespace gg
