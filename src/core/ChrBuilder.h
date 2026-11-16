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

#include <functional>

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

    // Extrait le premier bloc de code « chronomodel » (ou « json » contenant "events") d'un texte. Une clé écrite deux fois
    // au premier niveau (deux "events" : QJsonDocument ne garderait que le dernier) et un Event écrit deux fois sous le
    // même nom sont réunis ; `notes` reçoit alors une phrase par réunion (prompt réel du 13/11/2026).
    static bool extractSpec(const QString& markdown, QJsonObject& spec, QString* err = nullptr, QStringList* notes = nullptr);

    // Texte d'une réponse tel qu'affiché : le bloc de modèle (```chronomodel, ou ```json avec "events") est remplacé par
    // un résumé (« Modèle ChronoModel prêt : 2 événements… ») ou, si le modèle est incomplet, par la liste de ce qui
    // manque ; un bloc encore ouvert (réponse en cours) devient « Préparation du modèle… ». `showJson` : l'utilisateur a
    // demandé le JSON, le bloc reste visible (avec le résumé).
    static QString displayMarkdown(const QString& markdown, bool showJson = false);
    // Texte pour l'utilisateur : noms de champs, de réglages et d'outils (« event_constraints », « known_fixed »,
    // « calibrer_radiocarbone ») remplacés par des mots courants ; blocs de code inchangés.
    static QString humanizeNames(const QString& markdown);
    // Retire les blocs de modèle (```chronomodel, ou ```json avec "events") d'une réponse qui n'en demandait pas, avec
    // la ligne d'annonce qui les précède (« **Modèle ChronoModel** (à exporter) : ») et un séparateur --- resté seul.
    static QString stripModelBlocks(const QString& markdown);
    // La question demande explicitement à voir le JSON / le bloc du modèle.
    static bool asksForJson(const QString& question);
    // Résumé compact d'un .chr existant, injectable dans un prompt (interprétation, relecture).
    static QString summarize(const QJsonObject& chr, int maxEvents = 60);
    // Durée d'une phase d'un .chr, comme ChronoModel l'affiche (PhaseItem::getTauString) : « Durée ≤ 20 » (tau_type 1),
    // « Span uniforme » (tau_type 2), vide si elle est inconnue (tau_type 0, rien à signaler).
    static QString durationText(const QJsonObject& phase);
    // Hiatus d'une contrainte de phases d'un .chr (ArrowItem::getBubbleText) : « hiatus ≥ 50 » (gamma_type 1),
    // « hiatus min ∈ [a ; b] » (gamma_type 2), vide s'il est inconnu (gamma_type 0).
    static QString hiatusText(const QJsonObject& constraint);

    // Vérification croisée de la spécification écrite par le modèle de langage avec ce qu'a donné l'utilisateur
    // (`source` : ses questions et ses fichiers joints). Renvoie les écarts, en français :
    //   - valeur numérique d'une date ou d'une borne absente de la source (valeur inventée ou recopiée d'un exemple) ;
    //   - nom de date (référence) absent de la source (référence inventée) ;
    //   - « valeur ± erreur » ou référence de laboratoire (Ly-12345, Bdx-478) de la source absente du modèle (oubli).
    static QStringList crossCheck(const QJsonObject& spec, const QString& source);

    // Courbe âge-profondeur, convention de ChronoModel (Philippe, 12/11/2026) : la profondeur la plus PETITE (valeur
    // négative permise) est l'Event le plus ancien. Profondeurs données toutes positives ou nulles (sous la surface : « à 13 cm ») :
    // écrites en négatif (13 -> -13) ; dès qu'une valeur donnée est négative, les profondeurs sont signées et gardées
    // telles quelles (convention de ChronoModel, tableau CSV, cote sous un repère). `events` : « events » de la spécification.
    static bool signedDepths(const QJsonArray& events);
    // Même règle pour une spécification ; « depth_sign »: "as_given" à la racine (tableau CSV de ChronoModel, profondeur
    // mesurée depuis la base : 0 au fond, croissante avec le temps) : profondeurs toujours gardées telles quelles.
    static bool signedDepths(const QJsonObject& spec);
    // Valeur de X_Inc_Depth d'un Event de la spécification (« depth » ou « profondeur ») ; NaN sans profondeur.
    static double chronoDepth(const QJsonObject& event, bool signedDepths);

    // Modèle en mode courbe, étape 2 (16/11/2026) : le modèle de langage écrit les Events et leurs dates ; SeldonH recopie
    // lui-même, de `text` (questions et fichiers joints de l'utilisateur), les mesures de chaque Event sur l'événement :
    //   - archéomagnétisme (vector, spherical, unknown_dec, inclination, declination, intensity) : « I = 39,0° »,
    //     « D = -1,7° », « alpha95 = 6,1° », « F = 43,9 ± 6,1 µT » (aussi inclinaison, déclinaison, intensité, α95) ;
    //   - courbes univariate, bivariate, trivariate : « X = 12,3 ± 0,5 », « Y = … », « Z = … ».
    // Les mesures d'un Event sont lues entre son nom et le nom de l'Event suivant (ou une ligne vide). Une valeur écrite par
    // le modèle et différente du texte est remplacée par celle du texte. Une date « am » d'un Event qui recopie sa mesure
    // est retirée : en mode courbe, la mesure n'est pas une datation. `curveType` : type de la courbe (curve de la
    // spécification) ; « curve » est écrit à la racine s'il manque. `notes` : ce qui a été fait ou ce qui manque, en français.
    // Courbe âge-profondeur : rien (profondeurs traitées à part).
    // Mesures lues aussi dans un tableau (ligne d'en-tête « Nom ; I ; D ; alpha95 ; F ; sF », séparateurs ; tabulation | ou
    // espaces multiples, tableau Markdown compris) ; nom d'Event reconnu aussi sous une forme proche (« PI 2 » pour « PI02 »).
    // `missingEvents` : noms des Events (bornes exclues) auxquels il manque encore une composante.
    static QJsonObject fillCurveMeasures(QJsonObject spec, const QString& text, const QString& curveType, QStringList* notes = nullptr,
                                         QStringList* missingEvents = nullptr);
    // Recours (16/11/2026) : mesures renvoyées par le modèle de langage pour les Events incomplets,
    // {"PI07":{"inc":65.7,"dec":-8.3,"alpha95":4.8,"field":43,"error_f":4.9}} (clés de la fiche ou de ChronoModel). Une valeur
    // n'est prise que si `allowed` l'accepte (provenance : nombre écrit par l'utilisateur) et si l'Event ne l'a pas déjà.
    static QJsonObject mergeCurveMeasures(QJsonObject spec, const QJsonObject& byEvent, const QString& curveType,
                                          const std::function<bool(double)>& allowed, QStringList* notes = nullptr);
    // Le texte contient des mesures de courbe écrites par Event (au moins une ligne « nom … I = 39 » ou « X = 12 ») : le
    // modèle de langage peut alors laisser les mesures à SeldonH (réponse plus courte).
    static bool hasCurveMeasures(const QString& text, const QString& curveType);
    // Composantes demandées par un type de courbe : lettres parmi « IDF » (archéomagnétisme) ou « XYZ ». Vide si inconnu.
    static QString curveComponents(const QString& curveType);

    // Convention de contrainte : [a, b] = a plus ancien que b. ChronoModel stocke bwd_id (plus ancien) / fwd_id (plus récent) ;
    // sens vérifié sur trois projets réels.
    static constexpr bool kBwdIsOlder = true;
};

} // namespace seldon
