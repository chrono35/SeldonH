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

// SkillManager : base de "compétences" en Markdown, enrichissable par l'utilisateur.
//
// Une compétence est un fichier .md avec un en-tête :
//
//   ---
//   name: Modèle chronologique simple
//   description: Comment structurer un modèle ChronoModel (événements, phases, contraintes)
//   keywords: modèle, chronomodel, phase, stratigraphie, événement
//   always: false          # true = toujours injectée (ex. identité de l'assistant)
//   priority: 5            # départage les ex æquo (plus grand = prioritaire)
//   ---
//   Corps en Markdown...
//
// Les compétences intégrées sont dans le fichier de ressources (:/skills/*.md). Les compétences
// de l'utilisateur sont lues dans <données de l'application>/skills/*.md ; un fichier de même
// nom remplace la version intégrée. Pour enrichir l'assistant, il suffit donc de déposer un .md.
//
// La sélection est volontairement simple et déterministe (mots-clés, sans accents ni casse) :
// un petit modèle local n'a pas besoin d'un moteur de recherche vectoriel pour une centaine
// de fiches, et le résultat reste explicable ("pourquoi cette fiche ?").
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace seldon {

struct Skill {
    QString     id;            // nom de fichier sans extension
    QString     name;
    QString     description;
    QStringList keywords;      // normalisés (minuscules, sans accents)
    bool        always = false;
    int         priority = 0;
    QString     body;
    QString     origin;        // "intégrée" | "utilisateur"
};

struct SkillMatch {
    const Skill* skill = nullptr;
    int          score = 0;
};

class SkillManager {
public:
    // Recharge intégrées + dossier utilisateur. Retourne le nombre de compétences chargées.
    int reload(const QString& userDir);

    const QList<Skill>& skills() const { return skills_; }
    QString userDir() const { return userDir_; }

    // Meilleures compétences thématiques pour la question, dans la limite de maxChars.
    // `exclude` : identifiants déjà présents dans le contexte récent (non réinjectés).
    // `pinned` : fiches toujours incluses en tête, même hors budget et même si elles figurent dans `exclude`
    // (ex. la fiche de création de modèle pour une demande de modèle) ; le budget sert ensuite aux autres.
    QList<SkillMatch> select(const QString& question, int maxChars,
                             const QStringList& exclude = {}, const QStringList& pinned = {}) const;

    // Texte des compétences "always" (identité, règles) : va dans le message système.
    QString alwaysText() const;

    // Bloc de texte prêt à injecter dans le prompt.
    static QString format(const QList<SkillMatch>& m);

    // Utilitaire : minuscules + suppression des accents.
    static QString normalize(const QString& s);

    // Analyse d'un fichier (public pour les tests).
    static bool parse(const QString& id, const QString& text, const QString& origin, Skill& out);

private:
    QList<Skill> skills_;
    QString      userDir_;
};

} // namespace seldon
