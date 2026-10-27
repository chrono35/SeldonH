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

#include "TlMethod.h"

#include <QDate>
#include <cmath>

#include "../common/TextUtil.h"

namespace seldon {

// Mots-clés qui signalent cette méthode dans une question (texte passé par fold()).
QRegularExpression TlMethod::context() const
{
    static const QRegularExpression re(R"((\btl\b|\bosl\b|\birsl\b|thermoluminescen|luminescence|\bt\.l\.))");
    return re;
}

QVector<QJsonObject> TlMethod::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);
    const QRegularExpression ctx = context();
    if (!ctx.match(q).hasMatch()) return out;

    static const QRegularExpression refRe(
        R"((?:annee\s+(?:de\s+)?(?:reference|mesure)|mesure[e]?\s+en|reference\s*[:=]?|refyear\s*[=:]?)"
        R"(|measured\s+in|measurement\s+(?:year|in)|year\s+of\s+(?:measurement|reference)|reference\s+year|ref\.?\s+year)\s*[:=]?\s*(\d{4}))");
    const auto rm = refRe.match(q);

    for (const text::ValueError& ve : text::findValueErrors(q)) {
        if (out.size() >= 6) break;
        double scale = 1.0;
        const QString after = q.mid(ve.start + ve.length, 12);
        if (QRegularExpression(R"(^\s*(ka|k\.a\.|kyr|milliers|thousand))").match(after).hasMatch()) scale = 1000.0;
        QJsonObject o;
        o["age"] = ve.value * scale;
        o["error"] = ve.error * scale;
        if (rm.hasMatch()) {
            o["ref_year"] = rm.captured(1).toInt();
        } else {
            o["ref_year"] = QDate::currentDate().year();
            o["ref_year_assumed"] = true;
        }
        out << o;
    }
    return out;
}

MethodResult TlMethod::compute(const QJsonObject& p) const
{
    const double age = p.value("age").toDouble(), err = p.value("error").toDouble();
    const double ref = p.value("ref_year").toDouble();
    if (!(err > 0.0)) return MethodResult::failure("l'erreur doit être strictement positive");

    const double centre = ref - age;
    const double tmin = centre - 5.0 * err, tmax = centre + 5.0 * err;
    const double step = std::abs(tmax - tmin) / 1001.0;
    const auto L = [&](double t) -> long double {
        const long double norm = (age - (ref - t)) / err;
        return std::exp(-0.5L * norm * norm) / err;
    };
    QStringList warn;
    if (p.value("ref_year_assumed").toBool())
        warn << QStringLiteral("année de mesure non précisée : %1 supposée (à indiquer, par ex. « mesuré en 2015 »)").arg(int(ref));
    return MethodResult::fromDensity(buildDensity(tmin, tmax, step, L, true), warn, false);
}

QString TlMethod::describe(const QJsonObject& p) const
{
    return QStringLiteral("TL/OSL : %1 ± %2 ans avant %3")
        .arg(text::numAuto(p.value("age").toDouble(), 1), text::numAuto(p.value("error").toDouble(), 1),
             QString::number(p.value("ref_year").toInt()));
}

} // namespace seldon
