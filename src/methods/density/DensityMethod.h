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

// Méthode « density » (plugin Density de ChronoModel) : densité tabulée lue dans un fichier du dossier Density (Calib/Density
// de ChronoModel, dossier de l'utilisateur) ou un chemin absolu. Format de PluginDensity::loadRefFile : lignes « // » de
// description, tmin, tmax, puis une valeur par ligne sur une grille régulière ; densité renormalisée, nulle hors de
// [tmin ; tmax]. Ce n'est pas une mesure : la densité est la datation elle-même (05/11/2026).
#pragma once

#include <QRegularExpression>

#include "../common/Method.h"

namespace seldon {

class DensityMethod : public Method {
public:
    QString id() const override { return QStringLiteral("density"); }
    QString title() const override { return QStringLiteral("Densité tabulée (plugin Density)"); }
    QStringList curveNames() const override;
    QVector<QJsonObject> parse(const QString& question) const override;
    QRegularExpression context() const override;
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;

    // Lit le fichier « curve » (ou « file ») de la demande : densité normalisée, chemin du fichier lu.
    static bool load(const QJsonObject& p, Density& out, QString& file, QString& error);
};

} // namespace seldon
