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

#include "../common/CurveStore.h"
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

// Âge des géologues (« 12 ± 1 ka », « 1,2 ± 0,1 Ma », « 850 ± 70 ans BP ») : la loi gaussienne de ChronoModel est en années
// calendaires signées : année = origine − âge, origine 1950 (ka BP, usage des géologues ; résultats affichés en ka BP) ou
// 2000 avec « b2k ». L'écart-type est mis à l'échelle. Le texte saisi est gardé.
void applyAgeUnit(QJsonObject& o, const text::AgeUnit& u)
{
    if (u.scale == 1.0 && !std::isfinite(u.origin)) return;
    const double v = o.value("age").toDouble(), e = o.value("error").toDouble();
    const double origin = std::isfinite(u.origin) ? u.origin : 1950.0;
    o["input"] = QStringLiteral("%1 ± %2 %3").arg(text::numAuto(v, 4), text::numAuto(e, 4), u.label);
    o["age"] = origin - v * u.scale;
    o["error"] = e * u.scale;
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
    // Courbe de référence du plugin Gauss nommée dans la question (« avec la courbe curve_x_gauss_ref.csv ») : mode curve ;
    // la valeur est alors une mesure, pas un âge (pas de conversion ka, Ma).
    const QString curve = CurveStore::findNamed(id(), q);

    // N(1200, 50) ou N(1200; 50)
    static const QRegularExpression nform(R"(\bn\s*\(\s*(-?\d+(?:[.,]\d+)?)\s*[,;]\s*(\d+(?:[.,]\d+)?)\s*\))");
    auto it = nform.globalMatch(q);
    while (it.hasNext() && out.size() < 6) {
        const auto m = it.next();
        QJsonObject o;
        o["age"] = text::toNumber(m.captured(1));
        o["error"] = text::toNumber(m.captured(2));
        o["mode"] = "none";
        if (curve.isEmpty()) applyAgeUnit(o, text::ageUnitAt(q, m.capturedEnd()));
        if (o["error"].toDouble() > 0) out << o;
    }
    if (out.isEmpty())
        for (const text::ValueError& ve : text::findValueErrors(q)) {
            if (out.size() >= 6) break;
            QJsonObject o;
            o["age"] = ve.value;
            o["error"] = ve.error;
            o["mode"] = "none";
            if (curve.isEmpty()) applyAgeUnit(o, text::ageUnitAt(q, ve.start + ve.length));
            out << o;
        }
    if (!curve.isEmpty())
        for (QJsonObject& o : out) { o["mode"] = "curve"; o["curve"] = curve; }
    return out;
}

QStringList GaussMethod::curveNames() const { return CurveStore::names(id()); }

QStringList GaussMethod::referenceCurves(const QJsonObject& p) const
{
    if (p.value("mode").toString() != QLatin1String("curve")) return {};
    QString c = p.value("curve").toString().toLower();
    if (c.endsWith(QLatin1String(".csv"))) c.chop(4);
    return { QStringLiteral("|gauss|") + c };
}

MethodResult GaussMethod::compute(const QJsonObject& p) const
{
    const double age = p.value("age").toDouble();
    const double error = p.value("error").toDouble();
    const QString mode = p.value("mode").toString("none");
    if (!(error > 0.0)) return MethodResult::failure("l'erreur doit être strictement positive");
    // Combinaison (« untruncated ») : vraisemblance entière, sur ±12σ, pour que le produit avec une autre datation
    // ne s'annule pas dans les queues.
    const bool untruncated = p.value("untruncated").toBool();
    const double kS = untruncated ? 12.0 : kSigma;

    double tmin, tmax, step;
    std::function<double(double)> g;

    if (mode == "curve") {
        QString name = p.value("curve").toString().trimmed().toLower();
        if (name.endsWith(QLatin1String(".csv"))) name.chop(4);
        if (name.isEmpty()) return MethodResult::failure(QStringLiteral("mode « curve » sans courbe de référence (« curve »)"));
        const RefCurve* c = CurveStore::get(id(), name, CurveFormat::Simple);
        if (!c) {
            const QStringList known = curveNames();
            return MethodResult::failure(QStringLiteral("courbe de référence Gauss inconnue : « %1 » (%2)")
                                             .arg(name, known.isEmpty() ? QStringLiteral("aucune courbe dans le dossier Gauss ; joindre le fichier "
                                                                                         "ou le placer dans Calib/Gauss de ChronoModel")
                                                                        : QStringLiteral("disponibles : ") + known.join(", ")));
        }
        const double var0 = error * error;
        const auto L = [=](double t) -> long double {
            double mu, sg;
            c->at(t, mu, sg);
            const long double v = static_cast<long double>(sg) * sg + var0;
            const long double d = age - mu;
            return expl(-0.5L * d * d / v) / sqrtl(v);
        };
        const double span = c->tmax() - c->tmin();
        const double st = std::clamp(c->minStep / 3.0, span / 50000.0, std::max(span / 2000.0, 1e-9));
        const Density d = buildDensity(c->tmin(), c->tmax(), st, L, !untruncated);
        if (!d.valid())
            return MethodResult::failure(QStringLiteral("mesure %1 ± %2 incompatible avec la courbe « %3 »")
                                             .arg(text::numAuto(age, 4), text::numAuto(error, 4), name));
        return MethodResult::fromDensity(d, {}, true);
    }
    if (mode == "eq") {
        const double a = p.value("a").toDouble(), b = p.value("b").toDouble(), c = p.value("c").toDouble();
        g = [=](double t) { return a * std::abs(t) * std::abs(t) + b * t + c; };

        const auto s1 = solveQuadratic(age - kS * error, a, b, c);
        const auto s2 = solveQuadratic(age + kS * error, a, b, c);
        if (std::isinf(s1.first) || std::isinf(s2.first))
            return MethodResult::failure("équation de calibration constante : domaine indéterminé");
        tmin = std::min(s1.first, s2.first);
        tmax = std::max(s1.second, s2.second);
        const double sp = std::min(std::abs(s2.first - s1.first), std::abs(s2.second - s1.second));
        step = (sp > 0.0 ? sp : (tmax - tmin)) / 1001.0;
        if (!(tmax > tmin)) return MethodResult::failure("l'équation de calibration n'a pas de solution réelle dans la plage de la mesure");
    } else {
        g = [](double t) { return t; };
        tmin = age - kS * error;
        tmax = age + kS * error;
        step = 2.0 * error / 1001.0;
    }

    const double var = error * error;
    const auto L = [&](double t) -> long double {
        const long double d = age - g(t);
        return expl(-0.5L * d * d / var) / sqrtl(var);
    };
    return MethodResult::fromDensity(buildDensity(tmin, tmax, step, L, !untruncated), {}, false);
}

QString GaussMethod::describe(const QJsonObject& p) const
{
    auto n = [](double v) { return text::numAuto(v, 3); };
    QString s = QStringLiteral("Gauss : N(%1 ; %2)").arg(n(p.value("age").toDouble()), n(p.value("error").toDouble()));
    if (p.contains("input")) s = QStringLiteral("Gauss : âge %1 → N(%2 ; %3)").arg(p.value("input").toString(), n(p.value("age").toDouble()),
                                                                                  n(p.value("error").toDouble()));
    if (p.contains("input") && p.value("mode").toString() != QLatin1String("curve"))
        return p.value("input").toString().contains(QLatin1String("b2k")) || p.value("input").toString().contains(QLatin1String("BP"))
                   ? s : s + QStringLiteral(" (âge compté avant 1950)");
    if (p.value("mode").toString() == "curve") {
        QString c = p.value("curve").toString();
        if (c.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive)) c.chop(4);
        s += QStringLiteral(" — courbe de référence %1").arg(c);
    } else if (p.value("mode").toString() == "eq")
        s += QStringLiteral(" — calibration g(t) = %1·t² + %2·t + %3")
                 .arg(n(p.value("a").toDouble()), n(p.value("b").toDouble()), n(p.value("c").toDouble()));
    else
        s += QStringLiteral(" — sans calibration (année calendaire signée)");
    return s;
}

} // namespace seldon
