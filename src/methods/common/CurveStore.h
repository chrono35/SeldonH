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

// CurveStore : charge (une seule fois par courbe) les courbes de référence.
// Ordre de recherche, le premier trouvé l'emporte :
//   1. dossiers ajoutés par setExtraRoots() — dossier des courbes de l'utilisateur, puis dossier « Calib » de
//      ChronoModel — avec un sous-dossier par méthode, comme ChronoModel : 14C, F14C, AM ;
//   2. courbes intégrées aux ressources Qt : :/methods/<id>/curves/<nom>.<ext>.
// Extensions : .14c pour c14 et f14c, .ref pour am (casse indifférente). Nom d'une courbe = nom du fichier sans
// extension, en minuscules.
#pragma once

#include <QString>
#include <QStringList>

#include "RefCurve.h"

namespace seldon {

enum class CurveFormat { Radiocarbon, Simple };

class CurveStore {
public:
    // Noms (sans extension, en minuscules) des courbes d'une méthode.
    static QStringList names(const QString& methodId);

    // Retourne nullptr si absente ou illisible. `asF14C` n'a de sens que pour le format Radiocarbon.
    static const RefCurve* get(const QString& methodId, const QString& name, CurveFormat fmt, bool asF14C = false);

    // Dossiers racines supplémentaires, par priorité décroissante (chacun contient 14C/, F14C/, AM/). Vide le cache :
    // une courbe modifiée ou ajoutée est relue au prochain calcul.
    static void setExtraRoots(const QStringList& roots);
    static QStringList extraRoots();
    // Sous-dossier d'une méthode dans un dossier racine (« 14C », « F14C », « AM »), comme dans ChronoModel.
    static QString subdir(const QString& methodId);
    // Fichier d'où vient une courbe (chemin, ou « :/… » pour une courbe intégrée) ; vide si inconnue.
    static QString sourceOf(const QString& methodId, const QString& name);
    // Nom d'une courbe connue de la méthode écrit dans un texte (minuscules, sans accents), en mot entier ; le plus
    // long l'emporte (« intcal98_fix » plutôt que « intcal98 »). `pos` reçoit sa position. Vide si aucun.
    static QString findNamed(const QString& methodId, const QString& foldedText, int* pos = nullptr);
};

} // namespace seldon
