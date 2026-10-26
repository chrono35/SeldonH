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

#include "UnifMethod.h"

#include <cmath>

#include "../common/TextUtil.h"

namespace seldon {

// Mots-clés qui signalent cette méthode dans une question (texte passé par fold()).
QRegularExpression UnifMethod::context() const
{
    static const QRegularExpression re(R"((uniforme|uniform|\bunif\s*\())");
    return re;
}

QVector<QJsonObject> UnifMethod::parse(const QString& question) const
{
    QVector<QJsonObject> out;
    const QString q = text::fold(question);
    if (!q.contains("uniform") && !QRegularExpression(R"(\bunif\s*\()").match(q).hasMatch()) return out;

    const QString num = QStringLiteral("(-?\\d+(?:[.,]\\d+)?)");
    const QRegularExpression forms[] = {
        QRegularExpression(QStringLiteral("(?:unif\\w*|\\bu)\\s*\\(\\s*") + num + QStringLiteral("\\s*[,;]\\s*") + num + QStringLiteral("\\s*\\)")),
        QRegularExpression(QStringLiteral("(?:entre|between)\\s+") + num + QStringLiteral("\\s+(?:et|and)\\s+") + num),
        QRegularExpression(QStringLiteral("\\b(?:de|from)\\s+") + num + QStringLiteral("\\s+(?:a|jusqu'a|to)\\s+") + num),
        QRegularExpression(QStringLiteral("min\\w*\\s*[=:]?\\s*") + num + QStringLiteral(".{0,20}?max\\w*\\s*[=:]?\\s*") + num),
    };
    for (const QRegularExpression& re : forms) {
        auto it = re.globalMatch(q);
        while (it.hasNext() && out.size() < 6) {
            const auto m = it.next();
            double a = text::toNumber(m.captured(1)), b = text::toNumber(m.captured(2));
            if (a > b) std::swap(a, b);
            QJsonObject o;
            o["min"] = a;
            o["max"] = b;
            out << o;
        }
        if (!out.isEmpty()) break;
    }
    return out;
}

MethodResult UnifMethod::compute(const QJsonObject& p) const
{
    const double mn = p.value("min").toDouble(), mx = p.value("max").toDouble();
    if (!(mx > mn)) return MethodResult::failure("il faut min < max");
    const double w = 1.0 / (mx - mn);
    const auto L = [&](double t) -> long double { return (t >= mn && t <= mx) ? w : 0.0; };
    // La grille couvre exactement [min, max] ; pas non tronqué (densité constante).
    return MethodResult::fromDensity(buildDensity(mn, mx, (mx - mn) / 51.0, L, false), {}, false);
}

QString UnifMethod::describe(const QJsonObject& p) const
{
    return QStringLiteral("Uniforme : de %1 à %2 (années calendaires signées)")
        .arg(text::numAuto(p.value("min").toDouble(), 3), text::numAuto(p.value("max").toDouble(), 3));
}

} // namespace seldon
