// SPDX-License-Identifier: GPL-3.0-or-later
// Bundled, unmodified copy of ../../include/vec3.hpp from the main
// moon-graze C++ engine, so this directory builds standalone. Keep in sync
// by hand if the original changes.
// moon-graze -- a from-first-principles grazing lunar occultation path tool.
// Copyright (C) 2026 -- see LICENSE for the full GPLv3 text.
#pragma once
#include <cmath>
#include <array>

namespace mg {

struct Vec3 {
    double x = 0, y = 0, z = 0;

    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }

    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double norm() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const {
        double n = norm();
        return (n > 0) ? Vec3{x / n, y / n, z / n} : Vec3{0, 0, 0};
    }
};

// 3x3 rotation matrix, row-major, applied as v' = M * v.
struct Mat3 {
    std::array<std::array<double, 3>, 3> m{};

    static Mat3 identity() {
        Mat3 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
        return r;
    }

    // Rotation about the X axis by angle theta (radians), active rotation
    // convention matching the classical R1(theta) used in the Taylor et al.
    // (2011) lunar libration formulas.
    static Mat3 R1(double theta) {
        double c = std::cos(theta), s = std::sin(theta);
        Mat3 r;
        r.m[0] = {1, 0, 0};
        r.m[1] = {0, c, s};
        r.m[2] = {0, -s, c};
        return r;
    }

    // Rotation about the Z axis by angle theta (radians): R3(theta).
    static Mat3 R3(double theta) {
        double c = std::cos(theta), s = std::sin(theta);
        Mat3 r;
        r.m[0] = {c, s, 0};
        r.m[1] = {-s, c, 0};
        r.m[2] = {0, 0, 1};
        return r;
    }

    Vec3 apply(const Vec3& v) const {
        return {
            m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z,
        };
    }

    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                double s = 0;
                for (int k = 0; k < 3; k++) s += m[i][k] * o.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }

    // Transpose == inverse for a pure rotation matrix.
    Mat3 transposed() const {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i];
        return r;
    }
};

constexpr double PI = 3.14159265358979323846;
inline double deg2rad(double d) { return d * PI / 180.0; }
inline double rad2deg(double r) { return r * 180.0 / PI; }

} // namespace mg
