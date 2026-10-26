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

#include "Method.h"

#include <algorithm>

#include "TextUtil.h"

namespace seldon {

MethodResult MethodResult::failure(const QString& why)
{
    MethodResult r;
    r.error = why;
    return r;
}

MethodResult MethodResult::fromDensity(const Density& d, QStringList warnings, bool checkEdges)
{
    MethodResult r;
    if (!d.valid()) {
        r.error = QStringLiteral("la mesure est incompatible avec la courbe de référence (probabilité nulle sur tout le domaine)");
        return r;
    }
    r.density = d;
    r.hpd95 = hpdIntervals(d, 95.0);
    r.hpd68 = hpdIntervals(d, 68.0);
    r.stats = densityStats(d);
    r.warnings = warnings;

    if (checkEdges) {
        const double vmax = *std::max_element(d.v.begin(), d.v.end());
        const size_t n = d.v.size();
        const size_t k = std::min<size_t>(3, n - 1);
        if (d.v.front() > 0.02 * vmax || d.v[k] > 0.05 * vmax)
            r.warnings << QStringLiteral("la densité atteint la limite ancienne du domaine de la courbe (%1) : résultat tronqué")
                              .arg(text::yearLabel(d.t.front()));
        if (d.v.back() > 0.02 * vmax || d.v[n - 1 - k] > 0.05 * vmax)
            r.warnings << QStringLiteral("la densité atteint la limite récente du domaine de la courbe (%1) : résultat tronqué")
                              .arg(text::yearLabel(d.t.back()));
    }
    r.ok = true;
    return r;
}

} // namespace seldon
