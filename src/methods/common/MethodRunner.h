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

// MethodRunner : orchestre les méthodes pour l'interface.
//   question  ->  demandes reconnues  ->  calculs  ->  fiche HTML + bloc de texte pour le modèle + courbes
// Le modèle de langage peut aussi demander un calcul lui-même avec un bloc de code « calcul » (JSON).
#pragma once

#include <functional>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include "DateFormat.h"
#include "Density.h"

namespace seldon {

struct RunOutcome {
    bool        found = false;      // au moins une demande exécutée (même en échec)
    int         count = 0;
    QString     noticeHtml;         // fiche affichée dans le dialogue
    QString     promptBlock;        // valeurs exactes transmises au modèle
    QJsonObject plot;               // bloc « plot » (vide si rien à tracer)
    QVector<QJsonObject> extraPlots; // figures supplémentaires dans leur propre onglet (traces MCMC d'un événement)
    QJsonObject chrSpec;            // modèle ChronoModel préparé par SeldonH (tableau, action « model ») : à proposer à l'enregistrement
    QVector<QJsonObject> requests;  // demandes exécutées (normalisées) : suites proposées sous la réponse
    QString     title;              // opération(s) réellement calculée(s), en clair (« Calibration 14C », « Durée entre … ») :
                                    // titre de la fiche, pour que l'utilisateur contrôle le choix de l'outil
};

// Ordre de deux dates INDÉPENDANTES (densités calibrées) : densité de l'écart B − A, f(x) = ∫ f_A(a) f_B(a + x) da,
// et P(A antérieure à B) = P(B − A > 0). Pour des dates liées par un modèle ChronoModel, voir les traces (op « order »).
Density differenceDensity(const Density& a, const Density& b);
double probabilityBefore(const Density& a, const Density& b);

class MethodRunner {
public:
    // Reconnaît les dates de la question (voir MethodRegistry::parse) et les calcule.
    // `format` : format d'affichage par défaut (Préférences) ; une demande explicite dans la question le remplace
    // (« donne le résultat en cal BP »).
    // Vrai si la question demande de CRÉER un modèle ChronoModel (« fais-moi un modèle avec deux phases… ») : les valeurs
    // citées (âges, mesures) sont alors des données du modèle, pas des datations à calculer une par une.
    static bool isModelRequest(const QString& question);
    // Courbe âge-profondeur ou tableau CSV au format d'import de ChronoModel (fiche 13 imposée).
    static bool isAgeDepthRequest(const QString& question);
    // Méthodes de datation citées dans une question (pour n'injecter que leurs fiches « Date — … ») :
    // "c14", "f14c", "tl", "am", "gauss" (gaussienne, uniforme, TPQ…), "density".
    static QStringList datingMethodsIn(const QString& question);

    // Une demande de création de modèle ne déclenche aucun calcul automatique (résultat vide).
    static RunOutcome analyzeQuestion(const QString& question, DateFormat format = DateFormat::BcAd);
    // Datations (demandes de calcul) lues dans une question, sans les calculer ; vide pour une demande de modèle.
    // Si rien n'est reconnu, les mots déclencheurs mal orthographiés sont corrigés (FuzzyWords) et la question corrigée est
    // relue ; `fixes` (facultatif) décrit alors les corrections (« « nivau » → « niveau » »), vide sinon.
    static QVector<QJsonObject> requestsFromQuestion(const QString& question, QString* fixes = nullptr);
    // Même lecture, sans correction des mots.
    static QVector<QJsonObject> requestsFromQuestionExact(const QString& question);
    // Question de suivi qui demande seulement un autre niveau de confiance (« et à 50 % ? ») : les datations
    // précédentes (`previous`), avec ce niveau. Vide si la question contient ses propres datations ou pas de niveau.
    static QVector<QJsonObject> followUpRequests(const QString& question, const QVector<QJsonObject>& previous,
                                                 DateFormat format = DateFormat::BcAd);
    // Période d'étude écrite dans la question : « entre 0 et 2000 », « de 500 av. J.-C. à 500 », « période d'étude
    // de -500 à 500 », « study period from 0 to 2000 », « between 0 and 2000 ». Valeurs telles qu'écrites (format
    // d'affichage ; « N av. J.-C. » / « N BC » donne l'année astronomique 1 − N). Faux si absente. « Entre X et Y » n'est pas une
    // période dans une question de loi uniforme ni dans le passage d'un décalage (wiggle).
    static bool periodFromQuestion(const QString& question, double& from, double& to);

    // Niveau de confiance des intervalles HPD demandé dans la question, en % (« HPD à 99 % », « niveau de confiance 90 »,
    // « intervalle à 99,7 % »). 0 si la question n'en précise pas (95 % par défaut) ou hors de [50 ; 100[.
    // L'α95 archéomagnétique n'est pas pris pour un niveau de confiance.
    static double requestedLevel(const QString& question);
    static constexpr double kDefaultLevel = 95.0;

    // Événement (Event de ChronoModel) estimé par MCMC à partir de plusieurs datations :
    //   {"method":"event", "name":"US 12", "dates":[{"method":"c14",…}, {"method":"tl",…, "wiggle":{…}}],
    //    "level":95, "tmin":-1000, "tmax":2000, "mcmc":{"num_iter":100000, "num_proc":4, "seeds":[…], …}}
    // Paramètres MCMC absents = valeurs par défaut de ChronoModel. "wiggle" (facultatif) : {"type":"fixed","value":v},
    // {"type":"range","min":a,"max":b} ou {"type":"gaussian","mean":m,"error":e}.
    // Vrai si la question demande de combiner ses datations en un événement (« calcule l'événement… », « combine »).
    static bool asksEvent(const QString& question);
    // Combinaison de datations d'une même date (« Combine » de ChronoModel, produit des vraisemblances), éventuellement
    // de méthodes différentes et avec décalages (wiggle) :
    //   {"method":"combine", "name":"Four 3", "dates":[{"method":"c14",…}, {"method":"am",…, "wiggle":{…}}],
    //    "level":95, "tmin":…, "tmax":…}
    // Vrai si la question demande de combiner ses datations (« combine », « combinaison ») sans parler d'événement
    // (« événement » → MCMC, voir asksEvent).
    static bool asksCombine(const QString& question);
    // Relation entre deux ou trois termes : un seul calcul « relation »
    //   {"method":"relation", "op":"duration"|"between"|"value"|"order"|"inside", "from":A, "to":B, "of":C, "level":95}
    // où chaque terme est une colonne des traces MCMC chargées (« US 12 », « Phase A End »), une datation indépendante
    // (objet {"method":"c14",…}, « combine » ou « event ») ou une année fixe. Densité d'un terme (value : of), durée
    // B − A (duration), date uniforme entre A et B (between), P(A antérieur à B) (order), P(C dans [A ; B]) (inside).
    // Calcul exact sur les densités s'il n'y a ni colonne ni « between », sinon tirage par tirage (colonnes : leurs
    // tirages ; datations : tirées par inverse de leur fonction de répartition). Les anciens noms « order » (a, b) et
    // « traces » sont des alias, avec leur opération par défaut (order ; duration, ou value avec « of »).
    // asksOrder : la question compare deux dates (« laquelle est la plus ancienne ? », « A est-elle antérieure à B ? »,
    // « quelle date est avant l'autre ? ») ; avec deux datations dans la question, demande « relation » op « order ».
    static bool asksOrder(const QString& question);
    // La question relie des dates : ordre, durée ou écart, date entre deux termes, appartenance à un intervalle.
    static bool asksRelation(const QString& question);
    // Traces MCMC chargées (traces::loaded) et colonnes citées dans la question, éventuellement avec des datations
    // écrites dans la question (relation mixte) : demande « relation ». Vide si aucune trace chargée ou rien de reconnu.
    //   « durée entre N(500) et N(200) » -> {"method":"relation","op":"duration","from":"N(500)","to":"N(200)"}
    //   « durée entre la fin de la phase A et Ly-1 2450 ± 30 BP » -> {"method":"relation","op":"duration",
    //      "from":"Phase A End","to":{"method":"c14","age":2450,"error":30,"name":"Ly-1"}}
    static QVector<QJsonObject> tracesFromQuestion(const QString& question);
    // Courbe de ChronoModel chargée (curves::splines ou curves::maps) et question sur cette courbe : demande « curve ».
    //   « vitesse entre -500 et 300 » -> {"method":"curve","op":"rate","from":"-500","to":"300"}
    //   « vitesse entre Y=1,5 et Y=24,2 » -> {"method":"curve","op":"rate_values","y1":1.5,"y2":24.2,"dim":"Y"}
    //   « probabilité que X = 10 en 200 » -> {"method":"curve","op":"value","t":"200","y":10}
    //   « à quelle date la courbe atteint 0 ? » -> {"method":"curve","op":"crossing","y":0}
    // Objet vide si aucune courbe n'est chargée ou si la question n'est pas reconnue (le modèle de langage s'en charge).
    static QJsonObject curveFromQuestion(const QString& question);
    // Relation avec des datations des questions précédentes : « combien de temps sépare la date précédente de 14C
    // 3660 ± 25 ? » (une datation dans la question + la dernière de la conversation), « quelle durée entre ces deux
    // dates ? » (les deux dernières). `previous` : datations de la conversation, de la plus ancienne à la plus récente.
    // Il faut un mot qui renvoie à ces dates (précédente, dernière, ces deux, les deux…) et une relation reconnue
    // (durée, ordre, date entre) ; vide sinon.
    // `previous` : datations des questions précédentes ; un objet {"_column": "<nom>"} désigne une colonne des traces jointes.
    static QVector<QJsonObject> relationWithPrevious(const QString& question, const QVector<QJsonObject>& previous);
    // Calibration inverse demandée dans la question : « quel âge 14C attendre pour un échantillon de 6300 av. J.-C. ? »,
    // « quelle inclinaison pour l'an 1200 ± 20 ? », « calibration inverse entre 800 et 900 ». Année en BC/AD lue comme
    // l'affichage (N av. J.-C. = 1 − N) ; t_error seulement s'il est écrit. Objet vide si ce n'en est pas une.
    static QJsonObject inverseFromQuestion(const QString& question);
    // Réponse du modèle à l'écran : blocs « calcul », blocs json de calcul et JSON de calcul écrit dans le texte
    // remplacés par une mention (« Calcul confié à SeldonH ») ; tout est montré si l'utilisateur a demandé le JSON.
    static QString hideCalcBlocks(const QString& markdown, bool showJson);
    // Dernier bloc de calcul COMPLET d'une réponse (```calcul, ou ```json qui contient une demande de calcul) : position de
    // début et de fin (après la clôture). Faux s'il n'y en a pas. Sert à arrêter la génération après la demande de calcul :
    // ce que le modèle écrit ensuite (résultat supposé) serait inventé.
    static bool lastCalcBlock(const QString& markdown, qsizetype* start, qsizetype* end);
    // Vrai si la question demande de superposer les densités (« sur le même graphe », « superposées ») : sinon
    // chaque densité est tracée sur sa propre ligne.
    static bool asksOverlay(const QString& question);
    // La question demande plus que le calcul : explication, interprétation, avis, comparaison, conseil (« explique »,
    // « que penser de », « est-ce cohérent », « why », « interpret »…). Sinon, un calcul fait par SeldonH est affiché seul
    // (mode calculatrice), sans réponse du modèle de langage.
    static bool asksCommentary(const QString& question);
    // Question sur l'utilisation de SeldonH (« comment faire une datation ? », « que sais-tu faire ? », « how do I… »).
    // L'appelant vérifie qu'elle ne contient pas de calcul (requestsFromQuestion vide). Réponse attendue : phrases en
    // langage naturel que l'utilisateur peut taper, ou renvoi à l'aide ; ni code, ni outil, ni calcul (05/11/2026).
    static bool asksHowTo(const QString& question);
    // Unité d'âge d'une datation TL/OSL ou gaussienne écrite en JSON (« "unit":"ka" », « "origin":"BP" ») : âge et erreur
    // ramenés en années (TL : avant l'année de mesure ; Gauss : année calendaire, origine 1950 (ka BP) ou 2000 avec
    // « "origin":"b2k" »). Les clés « unit » et « origin » sont retirées ; « input » garde la saisie.
    static QJsonObject withAgeUnit(QJsonObject d, const QString& method);
    // « Liste les courbes de réf », « quelles courbes de calibration sont disponibles ? » : SeldonH affiche lui-même la liste
    // complète (le modèle n'en recopiait qu'une partie, 05/11/2026).
    static bool asksCurveList(const QString& question);
    static QString helpHint();         // consigne ajoutée au message pour une telle question
    // Retire les blocs de code (```…```) d'une réponse : réponse d'aide, où un bloc ne serait qu'un exemple.
    static QString stripCodeBlocks(const QString& answer);
    // En-tête du bloc de résultat envoyé au modèle : les valeurs sont déjà affichées dans la fiche de SeldonH, le modèle
    // ne les recopie pas. externalResultHeader() : pour un assistant qui ne voit pas la fiche (serveur MCP).
    static QString shownResultHeader();
    // Titre lisible d'une demande de calcul (« Calibration 14C », « Event « Four » (MCMC, 3 datations) »), sans code.
    static QString operationTitle(const QJsonObject& request);
    // Marque placée en tête d'une fiche de calcul (commentaire HTML lu par ChatView, qui en fait le bandeau de la fiche).
    static QString cardMarker(const QString& title);
    static QString externalResultHeader();
    // La question demande explicitement les traces des chaînes MCMC (« avec les traces », « trace de θ », « trace plot »…).
    static bool asksTraces(const QString& question);
    // Couleur d'une date ou d'un événement : « color » ("#rrggbb" ou nom), ou red/green/blue (format .chr). Vide sinon.
    static QString colorOf(const QJsonObject& o);
    // Paramètres MCMC et période d'étude cités dans la question (« 200 000 itérations », « 8 chaînes », « graines 1;2 »).
    static QJsonObject mcmcFromQuestion(const QString& question, double* tmin = nullptr, double* tmax = nullptr);
    // Avancement des calculs longs (MCMC) : texte, fraction 0..1 ; retourner false interrompt. Vide par défaut.
    static void setProgressHandler(std::function<bool(const QString&, double)> handler);

    // Exécute des demandes déjà structurées (JSON avec "method"). Clé facultative "level" : niveau de confiance (%) des
    // intervalles HPD, 95 par défaut ; les HPD à 95 % et 68 % restent toujours données en complément.
    static RunOutcome run(const QVector<QJsonObject>& requests, DateFormat format = DateFormat::BcAd);

    // Blocs ```calcul ... ``` (ou ```json avec "method") d'une réponse du modèle : objet JSON, tableau d'objets ou
    // objets séparés par des virgules ; "method" obligatoire. Chaque demande est normalisée (normalizeRequest).
    static QVector<QJsonObject> extractCalcBlocks(const QString& answer);

    // Demandes JSON écrites dans la question (fragment collé d'un bloc chronomodel ou d'un .chr) :
    //   « calcul Event avec ces deux dates {"method":"c14","age":2570,"error":35}, {"method":"c14",…} ».
    static QVector<QJsonObject> jsonRequests(const QString& text);
    // Bloc plot dont une série (ou la racine) donne une loi au lieu de valeurs : {"law":"normal","mean":-500,"sd":40}
    // (mêmes lois et paramètres que density_file). Les points sont calculés par SeldonH (le modèle de langage ne sait pas
    // évaluer une densité point par point). Objet vide (et `err`) si une loi est invalide ; inchangé sans loi.
    static QJsonObject expandPlotLaws(const QJsonObject& plot, QString* err = nullptr);

    // Décalage (wiggle) écrit dans la question : « wiggle entre 100 et 300 » (range), « décalage de 20 ± 5 »
    // (gaussian), « wiggle de 20 ans » (fixed). Objet vide si aucun.
    // `start`/`length` (facultatifs) : passage reconnu dans la question, pour le masquer avant de lire les datations.
    static QJsonObject wiggleFromQuestion(const QString& question, int* start = nullptr, int* length = nullptr);

    // Met une demande (ou une datation d'événement) dans la forme attendue par les méthodes :
    //  - « method » en minuscules, alias (14c, radiocarbone, osl, archeomag…) et noms d'outils (calculer_evenement…) ;
    //  - datation au format .chr de ChronoModel ({"plugin_id":"14c","data":{"age":…,"error":…,"ref_curve":…}}) aplatie ;
    //  - nombres écrits en texte (« "35" », « "2,5" ») convertis.
    // Dates d'un « event » ou d'un « combine » sans nom (ou nommées comme l'événement) : le nom est repris de la date du
    // .chr qui a la même méthode et les mêmes valeurs (de préférence le même décalage), chaque date du .chr servant une
    // fois. `known` : dates des .chr de la conversation, au format du .chr ou déjà normalisées.
    static QJsonObject fillDateNames(QJsonObject request, const QVector<QJsonObject>& known);
    static QJsonObject normalizeRequest(QJsonObject request);
    // Noms de champs abandonnés (refYear, errorF, deltaR…) : SeldonH n'emploie que ceux du .chr (ref_year, error_f,
    // delta_r…). Message d'erreur pour le modèle de langage (« champ « refYear » inconnu : écrire « ref_year » »), vide si
    // tout va bien. Les datations d'un événement (« dates ») sont vérifiées aussi.
    static QString obsoleteFieldError(const QJsonObject& request);
};

} // namespace seldon
