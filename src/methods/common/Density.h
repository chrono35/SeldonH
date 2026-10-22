// Density : densité de probabilité calendaire sur une grille régulière, intervalles HPD et statistiques.
//
// Port de ChronoModel (CNRS, GPL-3.0-or-later) : Date::calibrate() (grille au pas de la courbe, aires par
// trapèzes, troncature à 1e-5 de chaque côté, aire ramenée à 1) et create_HPD_by_dichotomy()
// (src/utilities/StdUtilities.cpp). Sans dépendance Qt.
#pragma once

#include <functional>
#include <vector>

namespace seldon {

struct Density {
    std::vector<double> t;     // années calendaires (grille régulière)
    std::vector<double> v;     // densité (aire = 1)
    double step = 0.0;
    bool valid() const { return t.size() >= 2 && t.size() == v.size(); }
};

struct Interval {
    double from = 0.0;         // année la plus ancienne
    double to = 0.0;           // année la plus récente
    double prob = 0.0;         // part de la probabilité totale contenue dans cet intervalle (0..1)
};

struct DensityStats {
    double mode = 0.0, median = 0.0, mean = 0.0, sd = 0.0, q1 = 0.0, q3 = 0.0;
};

using LikelihoodFn = std::function<long double(double)>;

// Évalue L(t) de tmin à tmax (nombre de pas arrondi, pas corrigé), tronque les queues (1e-5) si `truncate`,
// normalise l'aire à 1. Retourne une densité non valide si L est nulle partout.
Density buildDensity(double tmin, double tmax, double step, const LikelihoodFn& L, bool truncate = true);

double densityArea(const Density& d);

// Intervalles HPD au niveau `thresholdPercent` (ex. 95), du plus ancien au plus récent.
std::vector<Interval> hpdIntervals(const Density& d, double thresholdPercent);

DensityStats densityStats(const Density& d);

// Probabilité cumulée jusqu'à l'année t (0..1).
double densityCdf(const Density& d, double year);

} // namespace seldon
