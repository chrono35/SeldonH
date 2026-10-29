/* ---------------------------------------------------------------------

Copyright © CNRS	2026

Authors :
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

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

// Lexicon : repérage multilingue des mots-clés d'une question. Les fiches et les détecteurs de SeldonH (phases,
// demande de modèle, dessin, méthodes de datation…) cherchent des mots français ou anglais ; une question en espagnol,
// en italien ou en allemand passe d'abord par ce lexique, qui remplace chaque terme connu par son équivalent français.
// Le texte obtenu sert seulement au repérage : le modèle de langage reçoit la question d'origine.
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {

class Lexicon {
public:
    // Lexique intégré (:/lexicon/lexique.txt), complété par « lexique.txt » du dossier utilisateur s'il existe.
    // Retourne le nombre de termes chargés.
    int reload(const QString& userDir = QString());
    // Ajoute les lignes « français = équivalent, … » d'un texte (public pour les tests).
    void addText(const QString& text);

    // Question repliée (minuscules, sans accents), termes étrangers remplacés par leur forme française.
    QString translate(const QString& question) const;

    int size() const { return int(words_.size() + prefixes_.size() + phrases_.size()); }

private:
    QHash<QString, QString> words_;                       // mot -> français
    QVector<QPair<QString, QString>> prefixes_;           // « mot* » -> français, du plus long au plus court
    QVector<QPair<QString, QString>> phrases_;            // expression de plusieurs mots -> français, idem
};

} // namespace seldon
