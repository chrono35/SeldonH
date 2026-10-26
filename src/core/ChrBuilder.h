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

// ChrBuilder : transforme une SPÉCIFICATION SIMPLIFIÉE (JSON court produit par le LLM) en un
// projet ChronoModel complet (.chr, JSON).
//
// Pourquoi ? Un petit modèle local écrit mal un JSON de 200 champs, mais il sait très bien
// remplir un JSON de 10 champs. Le code déterministe ajoute ensuite tous les champs obligatoires
// (identifiants, couleurs, positions, MCMC, plages tmin/tmax...), ce qui garantit un fichier
// valide quelle que soit la qualité du modèle.
//
// Spécification acceptée (tout est optionnel sauf `events`) :
// {
//   "name": "Site X",
//   "phases":  [ {"name": "Phase 1"} ],
//   "events":  [ {"name": "US 101", "phases": ["Phase 1"],
//                 "dates": [ {"type":"gauss","name":"Ech-1","age":-520,"error":35},
//                            {"type":"unif","name":"TPQ-TAQ","min":-600,"max":-450},
//                            {"type":"tl","name":"TL-1","age":488,"error":19,"ref_year":2025},
//                            {"type":"am","name":"Four","process":"IF","inc":65,"alpha95":1.5,
//                             "field":54.2,"error_f":0.8,"curve_i":"gal2002sph2014_i","curve_f":"eurw_2017_f"} ] },
//                {"name": "Fondation", "type":"bound", "known_fixed": -700} ],
//   "event_constraints": [ ["US 101", "US 102"] ],   // [plus ancien, plus récent]
//   "phase_constraints": [ ["Phase 1", "Phase 2"] ], // [plus ancienne, plus récente]
//   "settings": {"tmin": -1000, "tmax": 200, "step": 1},
//   "mcmc": {"num_iter": 100000}
// }
// Archéomagnétisme : "process" = I (inclinaison), D (déclinaison), F (intensité), ID (directionnel), IF ou IDF (vectoriel).
// I demande inc + alpha95 ; D demande dec + inc + alpha95 (l'erreur sur D dépend de I) ; F demande field + error_f.
// Les âges gauss/unif sont en années calendaires (négatif = avant J.-C.) ; pour « tl », « age » est un âge en années
// avant « ref_year » (année de la mesure). Les champs écrits (échantillonneurs, MCMC, courbe, clés des dates et des
// événements) reprennent ceux de projets ChronoModel 4.0 réels.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace seldon {

struct ChrResult {
    bool        ok = false;
    QJsonObject chr;           // document .chr complet
    QStringList errors;        // bloquantes
    QStringList warnings;      // à montrer à l'utilisateur
    QString     suggestedName; // nom de fichier proposé (sans extension)
};

class ChrBuilder {
public:
    static ChrResult build(const QJsonObject& spec);

    // Extrait le premier bloc de code « chronomodel » (ou « json » contenant "events") d'un texte.
    static bool extractSpec(const QString& markdown, QJsonObject& spec, QString* err = nullptr);

    // Texte d'une réponse tel qu'affiché : le bloc de modèle (```chronomodel, ou ```json avec "events") est remplacé par
    // un résumé (« Modèle ChronoModel prêt : 2 événements… ») ou, si le modèle est incomplet, par la liste de ce qui
    // manque ; un bloc encore ouvert (réponse en cours) devient « Préparation du modèle… ». `showJson` : l'utilisateur a
    // demandé le JSON, le bloc reste visible (avec le résumé).
    static QString displayMarkdown(const QString& markdown, bool showJson = false);
    // La question demande explicitement à voir le JSON / le bloc du modèle.
    static bool asksForJson(const QString& question);
    // Résumé compact d'un .chr existant, injectable dans un prompt (interprétation, relecture).
    static QString summarize(const QJsonObject& chr, int maxEvents = 60);

    // Convention de contrainte : [a, b] = a plus ancien que b. ChronoModel stocke bwd_id (plus ancien) / fwd_id (plus récent) ;
    // sens vérifié sur trois projets réels.
    static constexpr bool kBwdIsOlder = true;
};

} // namespace seldon
