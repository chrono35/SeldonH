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

// Méthode « Gauss » : mesure gaussienne N(age, error), sans calibration (mode "none") ou avec une équation
// de calibration g(t) = a t² + b t + c (mode "eq").  Formule de ChronoModel (plugin_gauss) :
//   L(t) = exp( -½ (age - g(t))² / error² ) / error      (g(t) = t sans calibration)
// Domaine : age ± 5 σ (mode none) ou intervalle où g(t) ∈ age ± 5 σ (mode eq).
#pragma once

#include <QRegularExpression>

#include "../common/Method.h"

namespace seldon {

class GaussMethod : public Method {
public:
    QString id() const override { return QStringLiteral("gauss"); }
    QString title() const override { return QStringLiteral("Gauss"); }
    QVector<QJsonObject> parse(const QString& question) const override;
    QRegularExpression context() const override;
    bool usesValueErrors() const override { return true; }
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;
};

} // namespace seldon
