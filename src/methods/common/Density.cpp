/* ---------------------------------------------------------------------

Copyright © CNRS	2014 - 2026

Authors :
	Philippe LANOS
	Helori LANOS
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This file contains dating methods derived from ChronoModel
(https://chronomodel.com), distributed under the same license.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

SPDX-License-Identifier: GPL-3.0-or-later
--------------------------------------------------------------------- */

#include "Density.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace seldon {

namespace {

// Indice (fractionnaire) où la répartition croissante atteint `value`.
double indexForValue(double value, const std::vector<double>& rep)
{
    if (rep.empty()) return 0.0;
    if (value <= rep.front()) return 0.0;
    if (value >= rep.back()) return double(rep.size() - 1);
    const auto it = std::lower_bound(rep.begin(), rep.end(), value);
    const size_t k = size_t(it - rep.begin());
    const double a = rep[k - 1], b = rep[k];
    return double(k - 1) + (b > a ? (value - a) / (b - a) : 0.0);
}

double trapezeArea(const std::vector<double>& t, const std::vector<double>& v)
{
    double area = 0.0;
    for (size_t i = 1; i < t.size(); ++i)
        if (v[i - 1] > 0.0 && v[i] > 0.0) area += (v[i - 1] + v[i]) * 0.5 * (t[i] - t[i - 1]);
    return area;
}

double lerpX(double y, double y0, double y1, double x0, double x1)
{
    return (y1 == y0) ? x0 : x0 + (y - y0) * (x1 - x0) / (y1 - y0);
}

} // namespace

Density buildDensity(double tmin, double tmax, double step, const LikelihoodFn& L, bool truncate)
{
    Density out;
    if (!(tmax > tmin) || !(step > 0.0) || !std::isfinite(step)) return out;

    long nbStep = std::lround((tmax - tmin) / step);
    if (nbStep < 2) nbStep = 2;
    if (nbStep > 4000000) nbStep = 4000000;
    const long double longStep = static_cast<long double>(tmax - tmin) / nbStep;

    std::vector<double> vals(size_t(nbStep) + 1), rep(size_t(nbStep) + 1);
    long double acc = 0.0L, last = 0.0L;
    for (long i = 0; i <= nbStep; ++i) {
        const long double t = tmin + i * longStep;
        const long double v = L(static_cast<double>(t));
        vals[size_t(i)] = static_cast<double>(v);
        if (v != 0.0L && last != 0.0L) acc += (last + v);       // pas constant : facteur commun ignoré
        rep[size_t(i)] = static_cast<double>(acc);
        last = v;
    }
    if (!(rep.back() > 0.0) || !std::isfinite(rep.back())) return out;

    size_t i0 = 0, i1 = size_t(nbStep);
    if (truncate && rep.size() > 10) {
        const double thr = 0.00001;
        i0 = size_t(std::floor(indexForValue(thr * rep.back(), rep)));
        i1 = size_t(std::ceil(indexForValue((1.0 - thr) * rep.back(), rep)));
        if (i1 <= i0) { i0 = 0; i1 = size_t(nbStep); }
    }
    out.step = static_cast<double>(longStep);
    for (size_t i = i0; i <= i1; ++i) {
        out.t.push_back(static_cast<double>(tmin + static_cast<long double>(i) * longStep));
        out.v.push_back(vals[i]);
    }
    const double area = trapezeArea(out.t, out.v);
    if (!(area > 0.0)) return Density();
    for (double& x : out.v) x /= area;
    return out;
}

double densityArea(const Density& d) { return trapezeArea(d.t, d.v); }

std::vector<Interval> hpdIntervals(const Density& d, double thresholdPercent)
{
    std::vector<Interval> result;
    if (!d.valid()) return result;
    const size_t n = d.t.size();

    const double areaTot = trapezeArea(d.t, d.v);
    const double areaTarget = areaTot * thresholdPercent / 100.0;
    const double vmax = *std::max_element(d.v.begin(), d.v.end());

    if (std::abs(areaTot - areaTarget) < std::numeric_limits<double>::epsilon()) {
        result.push_back({ d.t.front(), d.t.back(), 1.0 });
        return result;
    }

    double vSup = vmax, vInf = 0.0;
    std::vector<Interval> lastIntervals;
    for (int iteration = 0; iteration < 20; ++iteration) {
        const double thr = (vSup + vInf) / 2.0;
        std::vector<Interval> cur;
        double totalArea = 0.0, areaSincePrev = 0.0;

        bool open = d.v[0] >= thr;          // (ChronoModel ouvre ici l'intervalle quand v0 < seuil : inversion corrigée)
        double curFrom = d.t[0];
        for (size_t k = 1; k < n; ++k) {
            const double pv = d.v[k - 1], nv = d.v[k];
            if (pv < thr && nv >= thr) {
                curFrom = lerpX(thr, pv, nv, d.t[k - 1], d.t[k]);
                open = true;
            }
            if (nv >= thr) {
                if (!open) { curFrom = d.t[k - 1]; open = true; }
                totalArea += (pv + nv) * (d.t[k] - d.t[k - 1]) / 2.0;
            }
            if (open && pv >= thr && nv < thr) {
                const double to = lerpX(thr, pv, nv, d.t[k - 1], d.t[k]);
                cur.push_back({ curFrom, to, (totalArea - areaSincePrev) });
                areaSincePrev = totalArea;
                open = false;
            }
        }
        if (open) cur.push_back({ curFrom, d.t.back(), (totalArea - areaSincePrev) });

        if (totalArea > areaTarget) vInf = thr; else vSup = thr;
        lastIntervals = cur;
        if (std::abs(totalArea - areaTarget) < areaTarget * 0.001) break;
    }
    for (Interval& iv : lastIntervals) iv.prob = areaTot > 0.0 ? iv.prob / areaTot : 0.0;
    return lastIntervals;
}

double densityCdf(const Density& d, double year)
{
    if (!d.valid()) return 0.0;
    const double tot = trapezeArea(d.t, d.v);
    if (!(tot > 0.0)) return 0.0;
    if (year <= d.t.front()) return 0.0;
    double acc = 0.0;
    for (size_t k = 1; k < d.t.size(); ++k) {
        if (year >= d.t[k]) {
            acc += (d.v[k - 1] + d.v[k]) * 0.5 * (d.t[k] - d.t[k - 1]);
        } else {
            const double u = year - d.t[k - 1];
            const double vy = d.v[k - 1] + (d.v[k] - d.v[k - 1]) * u / (d.t[k] - d.t[k - 1]);
            acc += (d.v[k - 1] + vy) * 0.5 * u;
            break;
        }
    }
    return std::min(1.0, acc / tot);
}

DensityStats densityStats(const Density& d)
{
    DensityStats s;
    if (!d.valid()) return s;
    const size_t n = d.t.size();

    // Mode, moyenne, écart-type : mêmes définitions que ChronoModel (analyseDensity) : somme discrète pondérée,
    // et au milieu d'un plateau quand le maximum est atteint sur plusieurs points consécutifs.
    {
        double ySum = 0.0, mean = 0.0, variance = 0.0, maxV = 0.0, prevY = 0.0;
        double plateauFrom = 0.0;
        bool inPlateau = false;
        for (size_t k = 0; k < n; ++k) {
            const double x = d.t[k], y = d.v[k];
            if (ySum == 0.0 && y == 0.0) continue;          // zéros initiaux
            ySum += y;
            const double meanPrev = mean;
            mean += (y / ySum) * (x - meanPrev);
            variance += y * (x - meanPrev) * (x - mean);
            if (maxV <= y) {
                maxV = y;
                if (prevY == y) {
                    if (!inPlateau) { plateauFrom = d.t[k - 1]; inPlateau = true; }
                    s.mode = (plateauFrom + x) / 2.0;
                } else {
                    inPlateau = false;
                    s.mode = x;
                }
            }
            prevY = y;
        }
        s.mean = mean;
        s.sd = ySum > 0.0 ? std::sqrt(variance / ySum) : 0.0;
    }

    std::vector<double> cdf(n, 0.0);
    for (size_t k = 1; k < n; ++k)
        cdf[k] = cdf[k - 1] + (d.v[k - 1] + d.v[k]) * 0.5 * (d.t[k] - d.t[k - 1]);
    const double tot = cdf.back();
    if (!(tot > 0.0)) return s;

    auto quantile = [&](double p) {
        const double target = p * tot;
        const auto it = std::lower_bound(cdf.begin(), cdf.end(), target);
        const size_t k = size_t(it - cdf.begin());
        if (k == 0) return d.t.front();
        if (k >= n) return d.t.back();
        const double a = cdf[k - 1], b = cdf[k];
        return d.t[k - 1] + (b > a ? (target - a) / (b - a) : 0.0) * (d.t[k] - d.t[k - 1]);
    };
    s.q1 = quantile(0.25);
    s.median = quantile(0.5);
    s.q3 = quantile(0.75);
    return s;
}

} // namespace seldon
