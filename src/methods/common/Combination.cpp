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

#include "Combination.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "../c14/C14Method.h"

namespace seldon {

namespace {

constexpr double kRad = 3.14159265358979323846 / 180.0;

// Densité au point t, interpolée linéairement sur la grille régulière ; 0 hors domaine.
double densityAt(const Density& d, double t)
{
    if (t < d.t.front() || t > d.t.back()) return 0.0;
    const double f = (t - d.t.front()) / d.step;
    const size_t i = std::min(size_t(f), d.t.size() - 2);
    const double r = f - double(i);
    return d.v[i] * (1.0 - r) + d.v[i + 1] * r;
}

// P(a, x) : fonction gamma incomplète régularisée inférieure (série, ou fraction continue de Lentz pour x ≥ a + 1).
double gammaP(double a, double x)
{
    if (!(x > 0.0)) return 0.0;
    const double lg = std::lgamma(a);
    if (x < a + 1.0) {
        double ap = a, sum = 1.0 / a, del = sum;
        for (int n = 0; n < 500; ++n) {
            ap += 1.0;
            del *= x / ap;
            sum += del;
            if (std::abs(del) < std::abs(sum) * 1e-15) break;
        }
        return std::clamp(sum * std::exp(-x + a * std::log(x) - lg), 0.0, 1.0);
    }
    constexpr double tiny = 1e-300;
    double b = x + 1.0 - a, c = 1.0 / tiny, d = 1.0 / b, h = d;
    for (int i = 1; i < 500; ++i) {
        const double an = -i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (std::abs(d) < tiny) d = tiny;
        c = b + an / c;
        if (std::abs(c) < tiny) c = tiny;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::abs(del - 1.0) < 1e-15) break;
    }
    return std::clamp(1.0 - std::exp(-x + a * std::log(x) - lg) * h, 0.0, 1.0);
}

struct Measure { double x = 0.0, v = 0.0; QString name; };

} // namespace

double chi2Cdf(double x, int df)
{
    return df > 0 ? gammaP(0.5 * df, 0.5 * x) : 0.0;
}

double chi2Quantile(double p, int df)
{
    if (df <= 0 || !(p > 0.0) || !(p < 1.0)) return std::nan("");
    double lo = 0.0, hi = std::max(10.0, 5.0 * df);
    while (chi2Cdf(hi, df) < p) hi *= 2.0;
    for (int i = 0; i < 200 && hi - lo > 1e-12 * hi; ++i) {
        const double mid = 0.5 * (lo + hi);
        (chi2Cdf(mid, df) < p ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

Density combineDensities(const std::vector<Density>& ds, QString* error)
{
    auto fail = [&](const QString& why) { if (error) *error = why; return Density(); };
    if (ds.empty()) return fail(QStringLiteral("aucune datation à combiner"));
    double lo = -std::numeric_limits<double>::infinity(), hi = std::numeric_limits<double>::infinity();
    double step = std::numeric_limits<double>::infinity();
    std::vector<Density> grid = ds;
    for (Density& d : grid)
        if (!(d.step > 0.0) && d.t.size() >= 2) d.step = (d.t.back() - d.t.front()) / double(d.t.size() - 1);
    for (const Density& d : grid) {
        if (!d.valid() || !(d.step > 0.0)) return fail(QStringLiteral("densité d'une datation invalide"));
        lo = std::max(lo, d.t.front());
        hi = std::min(hi, d.t.back());
        step = std::min(step, d.step);
    }
    if (!(hi > lo))
        return fail(QStringLiteral("les datations n'ont aucune période commune (leurs densités ne se recouvrent pas)"));
    // Grille commune : plus petit pas des datations, au plus 2 000 000 de points.
    long n = std::lround((hi - lo) / step);
    n = std::clamp(n, 2L, 2000000L);
    const double h = (hi - lo) / double(n);

    std::vector<double> logp(size_t(n) + 1, 0.0);
    double mx = -std::numeric_limits<double>::infinity();
    for (long i = 0; i <= n; ++i) {
        const double t = lo + double(i) * h;
        double s = 0.0;
        for (const Density& d : grid) {
            const double v = densityAt(d, t);
            if (!(v > 0.0)) { s = -std::numeric_limits<double>::infinity(); break; }
            s += std::log(v);
        }
        logp[size_t(i)] = s;
        mx = std::max(mx, s);
    }
    if (!std::isfinite(mx))
        return fail(QStringLiteral("le produit des vraisemblances est nul partout : les datations sont incompatibles "
                                   "(aucune date n'est plausible pour toutes à la fois)"));

    // Valeurs relatives au maximum (pas de débordement), puis troncature des queues à 1e-5 comme buildDensity.
    std::vector<double> v(logp.size()), rep(logp.size());
    double acc = 0.0, last = 0.0;
    for (size_t i = 0; i < logp.size(); ++i) {
        v[i] = std::exp(logp[i] - mx);
        if (v[i] != 0.0 && last != 0.0) acc += last + v[i];
        rep[i] = acc;
        last = v[i];
    }
    size_t i0 = 0, i1 = v.size() - 1;
    if (acc > 0.0 && v.size() > 10) {
        const double thr = 1e-5;
        while (i0 < i1 && rep[i0] < thr * acc) ++i0;
        while (i1 > i0 && rep[i1] > (1.0 - thr) * acc) --i1;
        i0 = i0 > 0 ? i0 - 1 : 0;
        i1 = std::min(v.size() - 1, i1 + 1);
    }
    Density out;
    out.step = h;
    for (size_t i = i0; i <= i1; ++i) { out.t.push_back(lo + double(i) * h); out.v.push_back(v[i]); }
    const double area = densityArea(out);
    if (!(area > 0.0)) return fail(QStringLiteral("produit des vraisemblances nul"));
    for (double& x : out.v) x /= area;
    return out;
}

std::vector<CompatibilityTest> wardWilsonTests(const std::vector<QJsonObject>& dates, const QStringList& labels, QStringList* skipped)
{
    // Clé de groupe -> (titre, unité, mesures)
    struct Group { QString title, unit; std::vector<Measure> m; };
    std::map<QString, Group> groups;
    auto add = [&](const QString& key, const QString& title, const QString& unit, double x, double var, const QString& name) {
        if (!std::isfinite(x) || !(var > 0.0)) return;
        Group& g = groups[key];
        g.title = title;
        g.unit = unit;
        g.m.push_back({ x, var, name });
    };
    for (size_t k = 0; k < dates.size(); ++k) {
        const QJsonObject& d = dates[k];
        const QString name = k < size_t(labels.size()) ? labels[int(k)] : QStringLiteral("Date %1").arg(k + 1);
        const QJsonValue w = d.value("wiggle");
        if (w.isDouble() || (w.isObject() && !w.toObject().value("type").toString().isEmpty()
                             && w.toObject().value("type").toString() != QLatin1String("none"))) {
            if (skipped) *skipped << name;
            continue;
        }
        const QString m = d.value("method").toString();
        if (m == QLatin1String("c14")) {
            // Âges conventionnels corrigés du réservoir : comparables seulement avec la même courbe.
            const QString curve = d.value("curve").toString("intcal20").toLower();
            const double e = d.value("error").toDouble(), re = d.value("delta_r_error").toDouble();
            add("c14|" + curve, QStringLiteral("âges 14C (%1)").arg(C14Method::curveTitle(curve)), QStringLiteral("BP"),
                d.value("age").toDouble() - d.value("delta_r").toDouble(), e * e + re * re, name);
        } else if (m == QLatin1String("f14c")) {
            const double e = d.value("error").toDouble();
            add("f14c", QStringLiteral("fractions F14C"), QString(), d.value("f14c").toDouble(), e * e, name);
        } else if (m == QLatin1String("tl")) {
            const double e = d.value("error").toDouble();
            add("cal", QStringLiteral("dates calendaires gaussiennes (Gauss, TL/OSL)"), QString(),
                d.value("ref_year").toDouble() - d.value("age").toDouble(), e * e, name);
        } else if (m == QLatin1String("gauss")) {
            const QString mode = d.value("mode").toString("none");
            if (mode != QLatin1String("none")) continue;                 // âge mesuré par une équation : grandeur propre
            const double e = d.value("error").toDouble();
            add("cal", QStringLiteral("dates calendaires gaussiennes (Gauss, TL/OSL)"), QString(), d.value("age").toDouble(), e * e, name);
        } else if (m == QLatin1String("am")) {
            const QString p = d.value("process").toString("I").toUpper();
            const double a95 = d.value("alpha95").toDouble(), inc = d.value("inc").toDouble();
            if (p.contains('I'))
                add("am_i|" + d.value("curve_i").toString().toLower(), QStringLiteral("inclinaisons"), QStringLiteral("°"),
                    inc, std::pow(a95 / 2.448, 2.0), name);
            if (p.contains('D'))
                add("am_d|" + d.value("curve_d").toString().toLower(), QStringLiteral("déclinaisons"), QStringLiteral("°"),
                    d.value("dec").toDouble(), std::pow(a95 / (2.448 * std::cos(inc * kRad)), 2.0), name);
            if (p.contains('F')) {
                const double e = d.value("error_f").toDouble();
                add("am_f|" + d.value("curve_f").toString().toLower(), QStringLiteral("intensités"), QStringLiteral("µT"),
                    d.value("field").toDouble(), e * e, name);
            }
        }
    }

    std::vector<CompatibilityTest> out;
    for (const auto& [key, g] : groups) {
        if (g.m.size() < 2) continue;
        double sw = 0.0, swx = 0.0;
        for (const Measure& x : g.m) { sw += 1.0 / x.v; swx += x.x / x.v; }
        CompatibilityTest t;
        t.group = g.title;
        t.unit = g.unit;
        t.mean = swx / sw;
        t.meanError = 1.0 / std::sqrt(sw);
        for (const Measure& x : g.m) { t.T += (x.x - t.mean) * (x.x - t.mean) / x.v; t.names << x.name; }
        t.df = int(g.m.size()) - 1;
        t.threshold = chi2Quantile(0.95, t.df);
        t.pValue = 1.0 - chi2Cdf(t.T, t.df);
        out.push_back(t);
    }
    return out;
}

} // namespace seldon
