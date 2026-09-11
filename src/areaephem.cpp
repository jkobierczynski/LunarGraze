// SPDX-License-Identifier: GPL-3.0-or-later
#include "areaephem.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace gg {

AreaEphemeris::AreaEphemeris(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open area ephemeris table: " + path);

    std::string line;
    long n = -1;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') continue;
        std::istringstream iss(line);
        iss >> n;
        break;
    }
    if (n < 0) throw std::runtime_error("malformed area ephemeris table (no row count): " + path);

    rows_.reserve(static_cast<size_t>(n));
    for (long i = 0; i < n; i++) {
        if (!std::getline(f, line))
            throw std::runtime_error("truncated area ephemeris table: " + path);
        std::istringstream iss(line);
        AreaEphemerisSample s;
        iss >> s.jd_tt >> s.moon.x >> s.moon.y >> s.moon.z
            >> s.sun.x >> s.sun.y >> s.sun.z
            >> s.earth_vel.x >> s.earth_vel.y >> s.earth_vel.z;
        if (!iss) throw std::runtime_error("malformed row " + std::to_string(i) + " in " + path);
        rows_.push_back(s);
    }
    if (rows_.size() < 4)
        throw std::runtime_error("area ephemeris table too short for interpolation: " + path);
    if (!std::is_sorted(rows_.begin(), rows_.end(),
                         [](auto& a, auto& b) { return a.jd_tt < b.jd_tt; }))
        throw std::runtime_error("area ephemeris table rows not sorted by time: " + path);
}

namespace {
double lagrange4(const double* xs, const double* ys, int n, double x) {
    double result = 0.0;
    for (int i = 0; i < n; i++) {
        double term = ys[i];
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            term *= (x - xs[j]) / (xs[i] - xs[j]);
        }
        result += term;
    }
    return result;
}
} // namespace

AreaEphemerisSample AreaEphemeris::at(double jd_tt) const {
    if (jd_tt < rows_.front().jd_tt || jd_tt > rows_.back().jd_tt)
        throw std::runtime_error(
            "requested time is outside the baked area ephemeris table's range "
            "-- regenerate it with gen_area_ephemeris.py for this date range");

    size_t lo = 0, hi = rows_.size() - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (rows_[mid].jd_tt <= jd_tt) lo = mid; else hi = mid;
    }
    long start = (long)lo - 1;
    if (start < 0) start = 0;
    if (start > (long)rows_.size() - 4) start = (long)rows_.size() - 4;

    double xs[4], mx[4], my[4], mz[4], sx[4], sy[4], sz[4], vx[4], vy[4], vz[4];
    for (int i = 0; i < 4; i++) {
        const auto& r = rows_[start + i];
        xs[i] = r.jd_tt;
        mx[i] = r.moon.x; my[i] = r.moon.y; mz[i] = r.moon.z;
        sx[i] = r.sun.x;  sy[i] = r.sun.y;  sz[i] = r.sun.z;
        vx[i] = r.earth_vel.x; vy[i] = r.earth_vel.y; vz[i] = r.earth_vel.z;
    }

    AreaEphemerisSample out;
    out.jd_tt = jd_tt;
    out.moon = mg::Vec3(lagrange4(xs, mx, 4, jd_tt), lagrange4(xs, my, 4, jd_tt), lagrange4(xs, mz, 4, jd_tt));
    out.sun  = mg::Vec3(lagrange4(xs, sx, 4, jd_tt), lagrange4(xs, sy, 4, jd_tt), lagrange4(xs, sz, 4, jd_tt));
    out.earth_vel = mg::Vec3(lagrange4(xs, vx, 4, jd_tt), lagrange4(xs, vy, 4, jd_tt), lagrange4(xs, vz, 4, jd_tt));
    return out;
}

} // namespace gg
