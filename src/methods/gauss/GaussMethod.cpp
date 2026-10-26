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

#include "GaussMethod.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../common/TextUtil.h"

namespace seldon {

namespace {

constexpr double kSigma = 5.0;

// y = a x² + b x + c  ->  racines triées (ChronoModel : solve_quadratic)
std::pair<double, double> solveQuadratic(double y, double a, double b, double c)
{
    const double inf = std::numeric_limits<double>::infinity();
    if (a == 0.0) {
        if (b == 0.0) return { inf, inf };
        const double r = (y - c) / b;
        return { r, r };
    }
    const double delta = b * b - 4.0 * a * (c - y);
    if (delta > 0.0) {
        const double s1 = (-b - std::sqrt(delta)) / (2.0 * a), s2 = (-b + std::sqrt(delta)) / (2.0 * a);
        return { std::min(s1, s2), std::max(s1, s2) };
    }
    return { 0.0, 0.0 };
}

} // namespace

// Mots-clés qui signalent cette méthode dans une question (texte passé par fold()).
QRegularExpression GaussMethod::context() const
{
    static const QRegularExpression re(R"((gauss|loi normale|distribution normale|normal distribution|\bn\s*\(|normale))");
    return re;
}

QVector<QJsonObject> GaussMethod::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);

    const QRegularExpression ctx = context();
    if (!ctx.match(q).hasMatch()) return out;

    // N(1200, 50) ou N(1200; 50)
    static const QRegularExpression nform(R"(\bn\s*\(\s*(-?\d+(?:[.,]\d+)?)\s*[,;]\s*(\d+(?:[.,]\d+)?)\s*\))");
    auto it = nform.globalMatch(q);
    while (it.hasNext() && out.size() < 6) {
        const auto m = it.next();
        QJsonObject o;
        o["age"] = text::toNumber(m.captured(1));
        o["error"] = text::toNumber(m.captured(2));
        o["mode"] = "none";
        if (o["error"].toDouble() > 0) out << o;
    }
    if (!out.isEmpty()) return out;

    for (const text::ValueError& ve : text::findValueErrors(q)) {
        if (out.size() >= 6) break;
        QJsonObject o;
        o["age"] = ve.value;
        o["error"] = ve.error;
        o["mode"] = "none";
        out << o;
    }
    return out;
}

MethodResult GaussMethod::compute(const QJsonObject& p) const
{
    const double age = p.value("age").toDouble();
    const double error = p.value("error").toDouble();
    const QString mode = p.value("mode").toString("none");
    if (!(error > 0.0)) return MethodResult::failure("l'erreur doit être strictement positive");

    double tmin, tmax, step;
    std::function<double(double)> g;

    if (mode == "eq") {
        const double a = p.value("a").toDouble(), b = p.value("b").toDouble(), c = p.value("c").toDouble();
        g = [=](double t) { return a * std::abs(t) * std::abs(t) + b * t + c; };

        const auto s1 = solveQuadratic(age - kSigma * error, a, b, c);
        const auto s2 = solveQuadratic(age + kSigma * error, a, b, c);
        if (std::isinf(s1.first) || std::isinf(s2.first))
            return MethodResult::failure("équation de calibration constante : domaine indéterminé");
        tmin = std::min(s1.first, s2.first);
        tmax = std::max(s1.second, s2.second);
        const double sp = std::min(std::abs(s2.first - s1.first), std::abs(s2.second - s1.second));
        step = (sp > 0.0 ? sp : (tmax - tmin)) / 1001.0;
        if (!(tmax > tmin)) return MethodResult::failure("l'équation de calibration n'a pas de solution réelle dans la plage de la mesure");
    } else {
        g = [](double t) { return t; };
        tmin = age - kSigma * error;
        tmax = age + kSigma * error;
        step = 2.0 * error / 1001.0;
    }

    const double var = error * error;
    const auto L = [&](double t) -> long double {
        const long double d = age - g(t);
        return expl(-0.5L * d * d / var) / sqrtl(var);
    };
    return MethodResult::fromDensity(buildDensity(tmin, tmax, step, L, true), {}, false);
}

QString GaussMethod::describe(const QJsonObject& p) const
{
    auto n = [](double v) { return text::numAuto(v, 3); };
    QString s = QStringLiteral("Gauss : N(%1 ; %2)").arg(n(p.value("age").toDouble()), n(p.value("error").toDouble()));
    if (p.value("mode").toString() == "eq")
        s += QStringLiteral(" — calibration g(t) = %1·t² + %2·t + %3")
                 .arg(n(p.value("a").toDouble()), n(p.value("b").toDouble()), n(p.value("c").toDouble()));
    else
        s += QStringLiteral(" — sans calibration (année calendaire signée)");
    return s;
}

} // namespace seldon
