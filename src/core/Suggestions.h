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
// Suggestions : questions cliquables proposées quand l'utilisateur joint un fichier (traces MCMC, journaux de ChronoModel,
// tableau de datations, projet .chr), construites avec les noms réels du fichier (Events, phases, méthodes). But : ne
// pas laisser l'utilisateur seul devant une zone de question vide ; chaque suggestion est comprise par SeldonH (calcul
// direct) ou par l'assistant (fiches de compétences). QtCore seul.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {
struct TraceSet;
struct SplineTrace;
struct CurveMap;
namespace chronoresults { struct Report; }
namespace datatable { struct Table; }

namespace suggest {

// Questions pour un export de chaînes MCMC (Chain_all_Events.csv, Chain_all_Phases.csv…), au plus `max`.
QStringList forTraces(const TraceSet& t, int max = 8);
// Questions pour une courbe de ChronoModel : trace des splines (Curve_X_Splines.csv) ou carte de densité (Curve_X_Map.csv).
// Dates et valeurs prises dans la courbe (quart et trois quarts de la période d'étude, médianes de la courbe).
QStringList forCurve(const SplineTrace& s, int max = 6);
QStringList forCurveMap(const CurveMap& m, int max = 3);
// Questions pour les journaux de résultats de ChronoModel (Log_Posterior_Distrib_Stats…).
QStringList forLog(const chronoresults::Report& r, int max = 5);
// Questions pour un tableau de datations lu par SeldonH.
QStringList forTable(const datatable::Table& t, int max = 6);
// Questions pour un projet ChronoModel (.chr) joint.
QStringList forChr(const QJsonObject& chr, int max = 4);

// Suites proposées sous la dernière réponse, d'après ce qui vient d'être calculé. Les « … » sont à compléter par
// l'utilisateur (une autre mesure). Affichage seulement : rien n'est envoyé au modèle de langage.
struct After {
    QVector<QJsonObject> requests;   // demandes exécutées par SeldonH pendant le tour (RunOutcome::requests)
    bool        modelBuilt = false;  // un modèle ChronoModel valide a été préparé
    QStringList traceColumns;        // colonnes des traces chargées
    const chronoresults::Report* log = nullptr;   // journaux de ChronoModel joints à la question
};
QStringList afterResult(const After& a, int max = 4);

// Bloc HTML « titre + liste de liens seldon://insert?q=… » (vide si `questions` est vide).
QString html(const QString& title, const QStringList& questions, const QString& footer = QString());

} // namespace suggest
} // namespace seldon
