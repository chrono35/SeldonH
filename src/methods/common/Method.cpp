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
