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

// Rapport de synthèse d'une conversation (13/11/2026), en texte (.txt) ou en Word (.docx). Deux parties :
//   - le DÉROULÉ, assemblé par SeldonH sans modèle de langage à partir du journal d'affichage : questions de l'utilisateur,
//     fiches de résultats de SeldonH (valeurs exactes), modèles ChronoModel préparés, figures, avertissements ;
//   - une SYNTHÈSE rédigée par le modèle de langage (facultative), étiquetée comme telle ; ses nombres sont contrôlés par
//     provenance::unsourcedNumbers contre le texte qu'il a reçu (promptFor), et les écarts listés dans le rapport.
// Les réponses de l'assistant ne sont pas recopiées dans le déroulé (l'export de la conversation les garde) : elles ne
// servent qu'au prompt de la synthèse.

#ifndef SELDON_REPORT_H
#define SELDON_REPORT_H

#include <QDateTime>
#include <functional>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ConversationStore.h"

namespace seldon {
namespace report {

struct Step {
    QString             question;      // texte affiché de la question (vide : notes avant la première question)
    QStringList         notices;       // fiches et notes de SeldonH (HTML, liens d'action retirés)
    QStringList         models;        // modèles ChronoModel préparés : résumé en clair
    QVector<QJsonObject> plots;        // figures (courbes ; scènes d'un .chr : titre seulement)
    QStringList         answers;       // réponses de l'assistant (Markdown affiché) : prompt de la synthèse seulement
};

struct Report {
    QString         title;
    QDateTime       created;
    QString         backend;           // moteur de la conversation (information)
    QVector<Step>   steps;
    QStringList     warnings;          // avertissements (« ⚠ … ») de toute la conversation, en texte
    QString         summary;           // synthèse du modèle de langage (Markdown), vide si aucune
    QString         summaryModel;      // modèle qui l'a rédigée
    QString         summaryNote;       // pourquoi il n'y a pas de synthèse (pas de modèle, arrêt, erreur), sinon vide
    QStringList     unsourced;         // nombres de la synthèse absents de ce que le modèle a reçu
};

// Déroulé à partir du journal d'affichage. `answers` : réponses brutes des tours (indice = DisplayItem::turn), pour
// retrouver les blocs ```chronomodel.
Report collect(const QVector<DisplayItem>& display, const QStringList& answers);

// Texte d'une fiche HTML : liens d'action (seldon://) retirés, lignes vides en trop réduites.
QString noticeText(const QString& html);

// Message pour le modèle de langage : consignes, puis le déroulé en texte (questions, résultats, modèles, réponses), dans
// un budget de caractères (les éléments longs sont raccourcis, les questions gardées).
QString promptFor(const Report& r, int budget);

// Synthèse lue dans la réponse du modèle : blocs de code retirés, espaces superflus enlevés.
QString cleanSummary(const QString& answer);

// Rapport en texte brut (UTF-8) et en HTML (pour QTextDocument puis l'écrivain .docx). `figureUrl(step, k)` : nom de la
// ressource image de la figure k de l'étape (vide : figure citée par son titre seulement).
QString toPlainText(const Report& r);
QString toHtml(const Report& r, const std::function<QString(int, int)>& figureUrl = {});

} // namespace report
} // namespace seldon

#endif
