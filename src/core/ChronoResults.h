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

// ChronoResults : résumé d'un export des résultats de ChronoModel (« Log_Posterior_Distrib_Stats.html », onglet
// Résultats > Log) pour le modèle de langage. Le fichier complet fait plusieurs centaines de milliers de caractères :
// SeldonH en extrait les valeurs exactes utiles au diagnostic (convergence, σ des dates), le modèle les commente.
//
// Lecture : blocs « Event : nom », « Data : nom », sous-blocs « Posterior Event Date », « Posterior Shrinkage param. »,
// « Posterior calib. date », « Posterior Std ti » ; critères « R̂ = … · ESS bulk = … · ESS tail = … ✅ » (une ligne) ou
// une ligne par critère avec ✅ / ⚠️ ; statistiques de densité « MAP = … ; Mean = … ; Std = … ».
// Phases (avant les Events) : « Phase: nom », « Number of Events: n », densités « Begin / End / Duration (posterior
// distrib.) », « Phase Time Range ( 95 %) : [ a ; b ] » ; puis « Succession : from A to B », « Min Hiatus … »,
// « Transition Range ( 95 %) : [ a ; b ] », « Gap Range ( 95 %) : [ a ; b ] » ou « No Gap ».
#pragma once

#include <QString>
#include <QVector>

namespace seldon {
namespace chronoresults {

struct Interval {
    double a = 0.0, b = 0.0;   // bornes, dans l'ordre du fichier (a < b)
    double p = 0.0;            // probabilité de ce morceau de HPD (%)
};

// Statistiques d'une densité a posteriori telles que ChronoModel les écrit (valeurs exactes du fichier).
struct Density {
    bool    has = false;                       // ligne « MAP = … ; Mean = … ; Std = … » lue
    double  map = 0.0, mean = 0.0, stdev = 0.0;
    double  q1 = 0.0, q2 = 0.0, q3 = 0.0;      // quartiles de la densité (ligne qui suit le MAP)
    double  min = 0.0, max = 0.0;              // extrêmes de l'échantillon (Trace Stat.)
    bool    hasMinMax = false;
    double  ciLevel = 0.0;   Interval ci;      // intervalle de crédibilité
    double  hpdLevel = 0.0;  QVector<Interval> hpd;   // région HPD, éventuellement en plusieurs morceaux
    QString unit;                              // « Age Cal. BP », « Years »…
    bool    sparse = false;                    // « Warning: Sample is too sparse »
};

struct Phase {
    QString  name;
    int      events = 0;
    Density  begin, end, duration;
    double   rangeLevel = 0.0;  Interval range;     // Phase Time Range
    bool     hasRange = false;
};

struct Succession {
    QString  from, to;          // phase la plus ancienne -> la plus récente
    QString  hiatus;            // hiatus minimal a priori (« unknown » s'il n'est pas fixé)
    double   level = 0.0;  Interval transition;  bool hasTransition = false;
    bool     gap = false;  Interval gapRange;    // « No Gap » : gap = false
    QString  unit;
};

struct Variable {
    QString kind;              // « date de l'événement », « σ de l'événement (shrinkage) », « date calibrée », « σ de la date (Std ti) »
    QString event;             // Event auquel appartient la variable
    QString data;              // Data (vide pour une variable d'Event)
    double  rhat = 0.0;
    int     essBulk = 0, essTail = 0;
    bool    rhatOk = true, bulkOk = true, tailOk = true;
    double  map = 0.0, stdev = 0.0;    // statistiques de la densité (MAP, écart-type)
    bool    hasStats = false;
    Density d;                         // densité complète
    bool ok() const { return rhatOk && bulkOk && tailOk; }
};

struct Report {
    QStringList header;        // projet, version, durées, critère global, lignes R̂ / ESS
    QVector<Variable> vars;
    int essThreshold = 1500;   // lu dans « (< 1500) » s'il est présent
    int events = 0, data = 0;
    QVector<Phase> phases;
    QVector<Succession> successions;
};

// Ce que demande une question sur des résultats déjà lus (drapeaux combinables).
enum Focus { FocusPhases = 1, FocusSuccessions = 2, FocusEvents = 4, FocusTable = 8 };

// HTML -> texte (balises de bloc -> retours à la ligne, entités décodées).
QString htmlToText(const QString& html);
// Vrai si le texte est un export de résultats de ChronoModel.
bool looksLike(const QString& text);
Report parse(const QString& text);
// Résumé pour le modèle (français) : critère global, variables hors critères de la pire à la meilleure, facteur
// d'itérations suggéré, dates au σ grand (outliers possibles), Events dont toutes les dates ont un σ grand.
QString summarize(const Report& r, const QString& fileName, int maxListed = 30);

// Drapeaux Focus de la question (0 : question de convergence ou sans rapport avec les phases et les Events).
int focusOf(const QString& question);
// Tableaux exacts (HTML) affichés par SeldonH : phases (ordre chronologique), successions, Events.
QString tablesHtml(const Report& r, int focus);
// Valeurs exactes pour le modèle, dans un budget de caractères : remarques calculées (phases à un seul Event,
// bornes de la période d'étude atteintes, HPD en plusieurs morceaux, gaps), puis une ligne par phase / succession /
// Event, en commençant par ceux que la question nomme. tableShown : le tableau complet est déjà affiché.
QString digest(const Report& r, int focus, const QString& question, int maxChars, bool tableShown);

} // namespace chronoresults
} // namespace seldon
