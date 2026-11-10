// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kyr4l
// The difficulty range each allod is expected to have, estimated from its own rated quests, so a quest that sticks out
// (too hard or too easy for its allod) is easy to spot.
//   - 4 or more rated quests: the median, plus or minus max(900, 1.2 x the median absolute deviation x 1.4826); the deviation
//     is robust, so one extreme quest does not stretch the range.
//   - fewer: estimated from the allods next to it (between the neighbours' medians, or one step beyond the last), +-1400.
//   - a range the user locked is used as it is.
// A quest within 150 points of a range's edge still counts as in range.
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace allodrange {

struct Range {
    double lo = 0, hi = 0;
    bool locked = false;
    bool estimated = false; // from the neighbours, not from the allod's own quests
    int rated = 0;
    double median = 0;
    bool hasMedian = false;
};

struct Lock { bool set = false; double lo = 0, hi = 0; };

inline double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[(n - 1) / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

inline double Round50(double v) { return std::round(v / 50.0) * 50.0; }

// scores[a]: the rated quests' scores of allod a (in the allods' order); locks[a]: the user's range, if any.
inline std::vector<Range> Compute(const std::vector<std::vector<double>>& scores, const std::vector<Lock>& locks) {
    const size_t n = scores.size();
    std::vector<Range> out(n);
    std::vector<double> autoLo(n, 0), autoHi(n, 0);
    for (size_t a = 0; a < n; ++a) {
        out[a].rated = static_cast<int>(scores[a].size());
        if (scores[a].empty()) continue;
        out[a].median = Median(scores[a]);
        out[a].hasMedian = true;
        if (scores[a].size() < 4) continue;
        std::vector<double> dev;
        for (double s : scores[a]) dev.push_back(std::fabs(s - out[a].median));
        const double half = std::max(900.0, 1.2 * Median(dev) * 1.4826);
        autoLo[a] = out[a].median - half;
        autoHi[a] = out[a].median + half;
    }
    for (size_t a = 0; a < n; ++a) {
        if (scores[a].size() >= 4) continue;
        bool havePrev = false, haveNext = false;
        double prev = 0, next = 0;
        for (size_t j = a; j-- > 0;) if (out[j].hasMedian) { prev = out[j].median; havePrev = true; break; }
        for (size_t j = a + 1; j < n; ++j) if (out[j].hasMedian) { next = out[j].median; haveNext = true; break; }
        const double c = havePrev && haveNext ? (prev + next) / 2 : havePrev ? prev + 1500 : haveNext ? next - 1500 : 5000;
        autoLo[a] = c - 1400;
        autoHi[a] = c + 1400;
        out[a].estimated = true;
    }
    for (size_t a = 0; a < n; ++a) {
        const bool locked = a < locks.size() && locks[a].set;
        out[a].locked = locked;
        out[a].lo = std::max(0.0, Round50(locked ? locks[a].lo : autoLo[a]));
        out[a].hi = std::min(10000.0, Round50(locked ? locks[a].hi : autoHi[a]));
    }
    return out;
}

enum class Verdict { InRange, TooHard, TooEasy };
inline Verdict Judge(double score, const Range& r, double* distance = nullptr) {
    if (distance) *distance = 0;
    if (score > r.hi + 150) { if (distance) *distance = score - r.hi; return Verdict::TooHard; }
    if (score < r.lo - 150) { if (distance) *distance = r.lo - score; return Verdict::TooEasy; }
    return Verdict::InRange;
}

} // namespace allodrange
