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

// InverseCalibration : mesure attendue pour une date calendaire, lue sur une courbe de référence (« calibration
// inverse »). Sert à préparer une campagne : quel âge 14C, quelle F14C, quelle inclinaison… mesurerait-on pour un
// échantillon de telle date, et avec quelle dispersion.
//
//   {"method":"inverse", "type":"c14"|"f14c"|"am", "t":-450, "t_error":25 | "t_min":-500,"t_max":-400,
//    "curve":"intcal20", "component":"I"|"D"|"F" (am), "lab_error":30, "level":95}
//
// La date est fixée (t), gaussienne (t ± t_error) ou uniforme sur [t_min ; t_max]. Pour chaque t, la mesure suit
// N(m(t), e(t)² + lab_error²) avec m, e la valeur et l'erreur (1σ) de la courbe ; la densité de la mesure est le
// mélange de ces lois sur la loi de t.
#pragma once

#include <QJsonObject>
#include <QString>

#include "Density.h"

namespace seldon {

struct InverseResult {
    bool    ok = false;
    QString error;
    QString quantity;               // « Âge 14C », « F14C », « Inclinaison »…
    QString unit;                   // « BP », « », « ° », « µT »
    QString curve;                  // nom de la courbe (minuscules)
    QString curveRef;               // « rôle|méthode|nom » (Method::referenceCurves)
    QString dateText;               // « -450 », « -450 ± 25 », « -500 → -400 » (années signées)
    double  tCentre = 0.0;          // date centrale
    double  curveMean = 0.0, curveError = 0.0;   // courbe à la date centrale
    double  mean = 0.0, sd = 0.0;   // loi de la mesure attendue (mélange)
    Density value;                  // densité de la mesure attendue (axe : valeur de la mesure)
    bool    hasF14c = false;        // 14C : équivalent en F14C
    double  f14cMean = 0.0, f14cSd = 0.0;
};

InverseResult inverseCalibration(const QJsonObject& request);

} // namespace seldon
