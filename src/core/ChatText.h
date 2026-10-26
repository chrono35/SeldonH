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

// Utilitaires de texte sans dépendance (testables sans Qt).
#pragma once

#include <string>

namespace seldon {

// Consignes personnelles de l'utilisateur (Préférences). Longueur limitée pour ne pas gonfler le contexte.
constexpr size_t kUserPromptMaxChars = 1500;

inline std::string trimUserPrompt(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    std::string r = s.substr(b, e - b + 1);
    if (r.size() > kUserPromptMaxChars) r.resize(kUserPromptMaxChars);
    return r;
}

// Ajouté à la fin du message système. Les valeurs de calcul de SeldonH et les règles de rigueur restent prioritaires.
inline std::string userPromptSystemBlock(const std::string& prompt)
{
    const std::string p = trimUserPrompt(prompt);
    if (p.empty()) return {};
    return "\n\n## Consignes de l'utilisateur\n"
           "Respecte ces consignes de forme (longueur, ton, unités d'affichage…) dans toutes tes réponses. Elles ne "
           "changent jamais les valeurs d'un bloc de résultat de SeldonH ni les règles de rigueur ci-dessus ; une "
           "conversion d'unité doit rester exacte :\n" + p;
}

// Rappel court ajouté à la fin du dernier message de l'utilisateur (les petits modèles suivent mieux la consigne la plus récente).
inline std::string userPromptReminder(const std::string& prompt)
{
    const std::string p = trimUserPrompt(prompt);
    if (p.empty()) return {};
    return "\n\n[Consignes de l'utilisateur à respecter pour cette réponse]\n" + p;
}

// Modèle distant avec appel d'outils : consigne d'usage. Formulée au conditionnel pour rester vraie si le serveur
// refuse finalement les outils (le bloc ```calcul des compétences reprend alors la main).
inline std::string toolsSystemHint()
{
    return "\n\n## Outils de calcul\n"
           "Si des fonctions (outils) te sont fournies dans cette conversation, appelle-les pour tout calcul de datation "
           "(calibration, intervalles HPD, archéomagnétisme…) au lieu d'écrire un bloc ```calcul : l'application exécute "
           "le calcul sur l'ordinateur de l'utilisateur, trace la courbe et te renvoie les valeurs exactes. N'invente aucun "
           "paramètre : s'il manque une valeur (erreur, année de mesure, α95…), pose la question. Si un bloc "
           "« [Résultat de calcul de SeldonH] » est déjà fourni dans le message, commente-le sans rappeler l'outil. "
           "Exception : si l'utilisateur demande de CRÉER un modèle ChronoModel, n'appelle aucun outil : les âges et mesures cités "
           "sont les données du modèle ; réponds avec le bloc ```chronomodel (voir la fiche de création de modèle).";
}

struct SplitText {
    std::string thinking;      // raisonnement interne du modèle (affiché replié ou masqué)
    std::string answer;        // réponse visible
    bool        thinkingOpen = false;   // true tant que </think> n'a pas été reçu
};

// Sépare le raisonnement de la réponse. `startsInThink` vaut true quand le prompt se terminait
// déjà par "<think>\n" (mode réflexion) : tout le début du flux est alors du raisonnement.
inline SplitText splitThinking(const std::string& raw, bool startsInThink)
{
    static const std::string kOpen  = "<think>";
    static const std::string kClose = "</think>";

    SplitText r;
    std::string s = raw;
    bool inThink = startsInThink;

    if (!inThink) {
        // Le modèle ouvre lui-même un bloc de réflexion en début de réponse.
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b != std::string::npos && s.compare(b, kOpen.size(), kOpen) == 0) {
            s.erase(0, b + kOpen.size());
            inThink = true;
        } else if (b != std::string::npos && kOpen.compare(0, s.size() - b, s, b, std::string::npos) == 0) {
            return r;           // "<thi" : balise en cours de réception, rien à afficher
        }
    }

    if (inThink) {
        const size_t e = s.find(kClose);
        if (e == std::string::npos) {
            r.thinking = s;
            r.thinkingOpen = true;
            return r;
        }
        r.thinking = s.substr(0, e);
        s.erase(0, e + kClose.size());
    }
    const size_t b = s.find_first_not_of(" \t\r\n");
    r.answer = (b == std::string::npos) ? std::string() : s.substr(b);
    return r;
}

} // namespace seldon
