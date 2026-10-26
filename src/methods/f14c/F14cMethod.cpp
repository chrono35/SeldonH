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

#include "F14cMethod.h"

#include <cmath>

#include "../c14/C14Method.h"
#include "../common/CurveStore.h"
#include "../common/TextUtil.h"

namespace seldon {

QStringList F14cMethod::curveNames() const { return CurveStore::names(id()) + CurveStore::names("c14"); }

const RefCurve* F14cMethod::resolveCurve(const QString& name)
{
    if (const RefCurve* c = CurveStore::get("f14c", name, CurveFormat::Radiocarbon, true)) return c;
    return CurveStore::get("c14", name, CurveFormat::Radiocarbon, true);
}

// Mots-clés qui signalent cette méthode dans une question (texte passé par fold()).
QRegularExpression F14cMethod::context() const
{
    static const QRegularExpression re(R"((f\s?-?14\s?c|fraction moderne|fraction 14c|post[- ]?bombe|bomb curve|post[- ]?bomb|fraction modern|modern fraction|\bpmc\b))");
    return re;
}

QVector<QJsonObject> F14cMethod::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);
    const QRegularExpression ctx = context();
    if (!ctx.match(q).hasMatch()) return out;

    // Courbe
    QString curve;
    static const QRegularExpression named(R"((bomb(?:21|13)(?:nhz|shz|nh|sh)\d{1,2}))");
    const auto nm = named.match(q);
    // Courbe nommée dans la question : courbe de la bombe, sinon courbe 14C (avant 1950).
    QString known = CurveStore::findNamed(QStringLiteral("f14c"), q);
    if (known.isEmpty()) known = CurveStore::findNamed(QStringLiteral("c14"), q);
    if (!known.isEmpty()) {
        curve = known;
    } else if (nm.hasMatch()) {
        curve = nm.captured(1);
    } else {
        const bool south = q.contains("hemisphere sud") || q.contains("australe") || q.contains("shcal")
                           || q.contains("southern hemisphere") || q.contains("south hemisphere");
        QString zone = "1";
        static const QRegularExpression z(R"(zone\s*([123]))");
        const auto zm = z.match(q);
        if (zm.hasMatch()) zone = zm.captured(1);
        curve = south ? QString("bomb21shz%1").arg(zone == "2" ? "1" : zone) : QString("bomb21nhz%1").arg(zone);
    }

    for (const text::ValueError& ve : text::findValueErrors(q)) {
        if (out.size() >= 6) break;
        QJsonObject o;
        o["f14c"] = ve.value;
        o["error"] = ve.error;
        // F14C < 1 (avant la bombe) : courbe d'âge convertie, sauf courbe explicitement nommée
        o["curve"] = (ve.value < 1.0 && !nm.hasMatch()) ? C14Method::defaultCurve(q) : curve;
        out << o;
    }
    return out;
}

MethodResult F14cMethod::compute(const QJsonObject& p) const
{
    const double F = p.value("f14c").toDouble();
    const double e = p.value("error").toDouble();
    const QString name = p.value("curve").toString("bomb21nhz1").toLower();
    if (!(e > 0.0)) return MethodResult::failure("l'erreur doit être strictement positive");
    if (!(F > 0.0)) return MethodResult::failure("le F14C doit être strictement positif");

    const RefCurve* c = resolveCurve(name);
    if (!c) return MethodResult::failure(QStringLiteral("courbe inconnue : « %1 » (disponibles : %2)").arg(name, curveNames().join(", ")));

    const double meas2 = e * e;
    const auto L = [&](double t) -> long double {
        double mu, sg;
        c->at(t, mu, sg);
        const long double v = static_cast<long double>(sg) * sg + meas2;
        const long double d = F - mu;
        return expl(-0.5L * d * d / v) / sqrtl(v);
    };
    const Density d = buildDensity(c->tmin(), c->tmax(), c->minStep / 3.0, L, true);
    if (!d.valid())
        return MethodResult::failure(QStringLiteral("F14C = %1 incompatible avec la courbe « %2 » (%3 → %4)")
                                         .arg(text::numAuto(F), name, text::yearLabel(c->tmin()), text::yearLabel(c->tmax())));
    QStringList warn;
    if (F > 1.0) warn << "un F14C > 1 donne souvent deux solutions (montée puis descente de la courbe de la bombe)";
    return MethodResult::fromDensity(d, warn, true);
}

QStringList F14cMethod::referenceCurves(const QJsonObject& p) const
{
    const QString name = p.value("curve").toString("bomb21nhz1").toLower();
    // Courbe de la bombe, ou courbe 14C (avant 1950) : même règle que resolveCurve().
    return { (CurveStore::sourceOf("f14c", name).isEmpty() ? QStringLiteral("|c14|") : QStringLiteral("|f14c|")) + name };
}

QString F14cMethod::describe(const QJsonObject& p) const
{
    const QString name = p.value("curve").toString("bomb21nhz1");
    return QStringLiteral("F14C : %1 ± %2 — %3")
        .arg(text::numAuto(p.value("f14c").toDouble(), 5), text::numAuto(p.value("error").toDouble(), 5),
             name.startsWith("bomb") ? name : C14Method::curveTitle(name));
}

} // namespace seldon
