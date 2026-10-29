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

// Combination : combinaison de datations d'une même date (bouton « Combine » de ChronoModel), étendue aux méthodes
// différentes.
//
// ChronoModel (PluginAbstract::getLikelihoodCombine) combine des dates d'un même plugin par le PRODUIT de leurs
// vraisemblances, chacune décalée par son wiggle. Les mesures de méthodes différentes (14C, TL, archéomagnétisme…)
// d'une même date étant indépendantes, le même produit s'applique entre méthodes : la densité combinée est
//   p(t) ∝ Π_i L_i(t),  avec  L_i(t) = ∫ L_i(t − δ) g_i(δ) dδ  pour une datation à décalage δ_i ~ g_i (wiggle).
// Chaque L_i est pris sur sa densité NON tronquée (méthode appelée avec « untruncated ») : une queue coupée à 1e-5
// annulerait le produit là où une autre datation est concentrée.
//
// Le test de Ward et Wilson (1978) n'est qu'une information sur l'accord des mesures, pas un critère de validité :
// il est calculé par groupe de mesures comparables (même grandeur, même courbe).
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

#include "Density.h"

namespace seldon {

// Produit des densités sur leur domaine commun (grille au plus petit pas, interpolation linéaire, calcul en
// logarithme), queues tronquées à 1e-5 comme buildDensity, aire ramenée à 1. Densité non valide si le domaine
// commun est vide ou le produit nul partout ; `error` en donne la raison.
Density combineDensities(const std::vector<Density>& densities, QString* error = nullptr);

// Test de Ward et Wilson pour un groupe de mesures d'une même grandeur : T = Σ (x_i − x̄)² / v_i, x̄ moyenne pondérée
// par 1/v_i, comparé au χ² à n − 1 degrés de liberté.
struct CompatibilityTest {
    QString     group;            // « 14C (IntCal20) », « dates calendaires gaussiennes (Gauss, TL) », « inclinaison »…
    QString     unit;             // « BP », « », « ° », « µT »
    QStringList names;            // datations du groupe
    double      mean = 0.0;       // moyenne pondérée
    double      meanError = 0.0;  // son écart-type, 1/sqrt(Σ 1/v_i)
    double      T = 0.0;
    int         df = 0;
    double      threshold = 0.0;  // χ²(df) au seuil de 5 %
    double      pValue = 0.0;     // P(χ²(df) ≥ T)
    bool        compatible() const { return T <= threshold; }
};

// Groupes de mesures comparables parmi les datations (requêtes normalisées : « method » et ses champs), un test par
// groupe d'au moins deux mesures. Une datation à décalage (wiggle) n'entre dans aucun groupe : sa mesure ne date pas
// directement l'événement. `skipped` reçoit les datations écartées pour cette raison.
std::vector<CompatibilityTest> wardWilsonTests(const std::vector<QJsonObject>& dates, const QStringList& labels,
                                               QStringList* skipped = nullptr);

// Loi du χ² : fonction de répartition et quantile (p dans ]0 ; 1[).
double chi2Cdf(double x, int df);
double chi2Quantile(double p, int df);

} // namespace seldon
