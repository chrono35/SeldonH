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

// MethodRegistry : liste des méthodes de datation disponibles (équivalent du PluginManager de ChronoModel).
#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

#include "Method.h"

namespace seldon {

class MethodRegistry {
public:
    static const MethodRegistry& instance();

    const QVector<const Method*>& methods() const { return methods_; }
    const Method* byId(const QString& id) const;

    // Demandes reconnues dans une question, dans l'ordre où elles y apparaissent. Si plusieurs méthodes sont citées
    // (« C14 2500 ± 30 BP et TL 488 ± 19 ans mesuré en 2015 »), chaque paire « valeur ± erreur » va à la méthode dont
    // le mot-clé la précède de plus près (ou la suit immédiatement, comme l'unité « BP »).
    QVector<QJsonObject> parse(const QString& question) const;

    // Calcule une demande JSON (clé "method").
    MethodResult compute(const QJsonObject& request) const;
    QString      describe(const QJsonObject& request) const;
    QStringList  referenceCurves(const QJsonObject& request) const;     // voir Method::referenceCurves

private:
    MethodRegistry();
    QVector<const Method*> methods_;
};

} // namespace seldon
