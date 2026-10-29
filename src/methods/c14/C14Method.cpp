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

#include "C14Method.h"

#include <cmath>

#include "../common/CurveStore.h"
#include "../common/TextUtil.h"

namespace seldon {

QStringList C14Method::curveNames() const { return CurveStore::names(id()); }

QString C14Method::curveTitle(const QString& name)
{
    QString n = name;
    if (n.startsWith("intcal")) return "IntCal" + n.mid(6);
    if (n.startsWith("shcal")) return "SHCal" + n.mid(5);
    if (n.startsWith("marine")) return "Marine" + n.mid(6);
    if (n == "uwsy98") return "UWSY98";
    return name;
}

QString C14Method::defaultCurve(const QString& q)
{
    // Courbe nommée dans la question (intégrée, de ChronoModel ou de l'utilisateur) : « intcal98_fix », « shcal13 »…
    if (const QString named = CurveStore::findNamed(QStringLiteral("c14"), q); !named.isEmpty()) return named;
    static const QRegularExpression explicitName(R"((intcal|shcal|marine)\s*-?\s*(20|13|09|04))");
    const auto m = explicitName.match(q);
    if (m.hasMatch()) return m.captured(1) + m.captured(2);

    QString family = "intcal";
    if (q.contains("marine") || q.contains("marin ") || q.contains("marins") || q.contains("coquille")
        || q.contains("poisson") || q.contains("mammifere marin") || q.contains("reservoir")
        || q.contains("shell") || q.contains("fish") || q.contains("marine mammal"))
        family = "marine";
    else if (q.contains("shcal") || q.contains("hemisphere sud") || q.contains("hemisphere austral")
             || q.contains("australe") || q.contains("sud de l'equateur")
             || q.contains("southern hemisphere") || q.contains("south hemisphere"))
        family = "shcal";
    return family + "20";
}

// Mots-clés qui signalent cette méthode dans une question (texte passé par fold()).
QRegularExpression C14Method::context() const
{
    static const QRegularExpression re(R"((c\s?-?14|14\s?-?c|carbone\s*14|carbon\s*-?\s*14|radiocarbon|\bbp\b|calibr|intcal|shcal|marine\s?(?:20|13|09|04)))");
    return re;
}

QVector<QJsonObject> C14Method::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);

    // Contexte radiocarbone obligatoire : évite de prendre n'importe quel « 120 ± 5 » pour une date 14C.
    const QRegularExpression ctx = context();
    if (!ctx.match(q).hasMatch()) return out;

    const QString curve = defaultCurve(q);

    // ΔR (réservoir)
    static const QRegularExpression dr(
        QStringLiteral("(?:delta\\s*r|\\bdr\\b|δr)\\s*[=:]?\\s*(-?\\d+(?:[.,]\\d+)?)(?:\\s*") + text::plusMinus()
        + QStringLiteral("\\s*(\\d+(?:[.,]\\d+)?))?"));
    const auto dm = dr.match(q);
    QString scan = q;
    if (dm.hasMatch()) scan.remove(dm.capturedStart(), dm.capturedLength());

    for (const text::ValueError& ve : text::findValueErrors(scan)) {
        if (ve.value < 0 || out.size() >= 6) continue;
        QJsonObject o;
        o["age"] = ve.value;
        o["error"] = ve.error;
        o["curve"] = curve;
        if (dm.hasMatch()) {
            o["delta_r"] = text::toNumber(dm.captured(1));
            o["delta_r_error"] = dm.captured(2).isEmpty() ? 0.0 : text::toNumber(dm.captured(2));
            o["has_delta_r"] = true;
        }
        out << o;
    }
    return out;
}

MethodResult C14Method::compute(const QJsonObject& p) const
{
    const double age = p.value("age").toDouble();
    const double error = p.value("error").toDouble();
    const double dR = p.value("delta_r").toDouble();
    const double dRerr = p.value("delta_r_error").toDouble();
    const QString name = p.value("curve").toString("intcal20").toLower();

    if (!(error > 0.0)) return MethodResult::failure("l'erreur doit être strictement positive");
    const RefCurve* c = CurveStore::get(id(), name, CurveFormat::Radiocarbon, false);
    if (!c) return MethodResult::failure(QStringLiteral("courbe de calibration inconnue : « %1 » (disponibles : %2)")
                                             .arg(name, curveNames().join(", ")));

    const double a = age - dR;
    const double meas2 = error * error + dRerr * dRerr;

    const auto L = [&](double t) -> long double {
        double mu, sg;
        c->at(t, mu, sg);
        const long double v = static_cast<long double>(sg) * sg + meas2;
        const long double d = a - mu;
        return expl(-0.5L * d * d / v) / sqrtl(v);
    };

    const Density d = buildDensity(c->tmin(), c->tmax(), c->minStep / 3.0, L, !p.value("untruncated").toBool());   // « untruncated » : combinaison
    QStringList warn;
    if (name.startsWith("marine") && !p.value("has_delta_r").toBool())
        warn << "courbe marine sans ΔR : la correction réservoir régionale n'est pas précisée (ΔR = 0 par défaut)";
    if (age < 0) warn << "âge conventionnel négatif : échantillon post-bombe, utiliser plutôt la méthode F14C";
    return MethodResult::fromDensity(d, warn, true);
}

QStringList C14Method::referenceCurves(const QJsonObject& p) const
{
    return { QStringLiteral("|c14|") + p.value("curve").toString("intcal20").toLower() };
}

QString C14Method::describe(const QJsonObject& p) const
{
    auto n = [](double v) { return text::numAuto(v, 1); };
    QString s = QStringLiteral("14C : %1 ± %2 BP — %3")
                    .arg(n(p.value("age").toDouble()), n(p.value("error").toDouble()),
                         curveTitle(p.value("curve").toString("intcal20")));
    if (p.value("has_delta_r").toBool())
        s += QStringLiteral(", ΔR = %1 ± %2").arg(n(p.value("delta_r").toDouble()), n(p.value("delta_r_error").toDouble()));
    return s;
}

} // namespace seldon
