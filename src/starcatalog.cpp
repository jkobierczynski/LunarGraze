// SPDX-License-Identifier: GPL-3.0-or-later
#include "starcatalog.hpp"
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cmath>
#include <cstdlib>

namespace gg {

namespace {

// Minimal CSV line splitter handling double-quoted fields (HYG's format;
// embedded commas inside quotes are rare in this catalogue but a proper
// star/Bayer/Flamsteed field could in principle have one -- handle it
// rather than assume it never happens).
std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; }
                else in_quotes = false;
            } else {
                cur += c;
            }
        } else {
            if (c == '"') in_quotes = true;
            else if (c == ',') { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// Standard J2000 mean-obliquity equatorial -> ecliptic latitude conversion.
// Epoch precision doesn't matter here: this is only used to classify
// "could the Moon ever occult this star" (|beta| < 8 deg), not for
// geometry that feeds the actual graze computation.
double ecliptic_latitude_deg(double ra_deg, double dec_deg) {
    constexpr double kObliquityDeg = 23.439291;
    double ra = mg::deg2rad(ra_deg), dec = mg::deg2rad(dec_deg), eps = mg::deg2rad(kObliquityDeg);
    double sin_beta = std::sin(dec) * std::cos(eps) - std::cos(dec) * std::sin(eps) * std::sin(ra);
    sin_beta = std::max(-1.0, std::min(1.0, sin_beta));
    return mg::rad2deg(std::asin(sin_beta));
}

double parse_double_or(const std::string& s, double dflt) {
    if (s.empty()) return dflt;
    try {
        size_t pos;
        double v = std::stod(s, &pos);
        return v;
    } catch (...) {
        return dflt;
    }
}

} // namespace

std::vector<mg::Star> load_hyg_catalog(const std::string& path, double mag_limit) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open HYG catalogue: " + path);

    std::string header_line;
    if (!std::getline(f, header_line))
        throw std::runtime_error("empty HYG catalogue: " + path);
    auto header = split_csv_line(header_line);
    std::unordered_map<std::string, int> col;
    for (size_t i = 0; i < header.size(); i++) col[header[i]] = static_cast<int>(i);

    auto need = [&](const char* name) -> int {
        auto it = col.find(name);
        if (it == col.end())
            throw std::runtime_error(std::string("HYG catalogue missing column '") + name + "': " + path);
        return it->second;
    };
    int i_ra = need("ra"), i_dec = need("dec"), i_mag = need("mag");
    int i_proper = need("proper"), i_bf = need("bf"), i_hd = need("hd"), i_hip = need("hip"), i_id = need("id");
    int i_pmra = need("pmra"), i_pmdec = need("pmdec");

    auto get = [](const std::vector<std::string>& row, int idx) -> std::string {
        return (idx >= 0 && idx < static_cast<int>(row.size())) ? row[idx] : std::string();
    };

    std::unordered_map<std::string, mg::Star> by_name; // last one wins, mirrors graze_finder.py
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto row = split_csv_line(line);

        std::string mag_s = get(row, i_mag);
        if (mag_s.empty()) continue;
        double mag;
        try { mag = std::stod(mag_s); } catch (...) { continue; }
        if (mag > mag_limit) continue;

        std::string proper = get(row, i_proper);
        if (proper == "Sol") continue; // not a real occultable "star"

        std::string ra_s = get(row, i_ra), dec_s = get(row, i_dec);
        if (ra_s.empty() || dec_s.empty()) continue;
        double ra, dec;
        try { ra = std::stod(ra_s); dec = std::stod(dec_s); } catch (...) { continue; }

        std::string name = proper;
        if (name.empty()) name = get(row, i_bf);
        if (name.empty()) name = get(row, i_hd);
        if (name.empty()) name = get(row, i_hip);
        if (name.empty()) name = get(row, i_id);
        if (name.empty()) name = "?";

        double beta = ecliptic_latitude_deg(ra * 15.0, dec); // ra column is HOURS in HYG
        if (std::fabs(beta) >= kEclipticLatCutoffDeg) continue;

        mg::Star s;
        s.name = name;
        s.ra_deg = ra * 15.0; // hours -> degrees
        s.dec_deg = dec;
        s.pmra_masyr = parse_double_or(get(row, i_pmra), 0.0);
        s.pmdec_masyr = parse_double_or(get(row, i_pmdec), 0.0);
        s.vmag = mag;
        by_name[name] = s;
    }

    std::vector<mg::Star> out;
    out.reserve(by_name.size());
    for (auto& [k, v] : by_name) out.push_back(v);
    return out;
}

} // namespace gg
