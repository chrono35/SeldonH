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

// Méthode « F14C » : calibration d'une fraction moderne (F14C), surtout pour les échantillons post-bombe
// (courbes de Hua et al. 2013/2021) ; les courbes d'âge IntCal/SHCal/Marine sont converties en F14C.
// Formule de ChronoModel (plugin_F14C) :  L(t) = exp( -½ (F - μ(t))² / v ) / √v ,  v = σ_courbe² + σ_mesure²
#pragma once

#include <QRegularExpression>

#include "../common/Method.h"
#include "../common/RefCurve.h"

namespace seldon {

class F14cMethod : public Method {
public:
    QString id() const override { return QStringLiteral("f14c"); }
    QString title() const override { return QStringLiteral("Fraction moderne (F14C)"); }
    QStringList curveNames() const override;
    QVector<QJsonObject> parse(const QString& question) const override;
    QRegularExpression context() const override;
    bool usesValueErrors() const override { return true; }
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;
    QStringList referenceCurves(const QJsonObject& p) const override;

    // Résout un nom de courbe : dossier f14c d'abord, puis courbes d'âge de c14 (converties).
    static const RefCurve* resolveCurve(const QString& name);
};

} // namespace seldon
