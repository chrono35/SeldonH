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

// Traces : chaînes MCMC exportées par ChronoModel (Résultats > Export : Chain_all_Events.csv, Chain_all_Phases.csv,
// Chain_Phase_<nom>.csv), et calculs a posteriori sur ces tirages.
//
// Format (Model::getEventsTraces / getPhasesTraces) : en-tête « iter » puis un nom par colonne (Event, ou
// « <phase> Begin » / « <phase> End ») ; une ligne par tirage d'acquisition ; chaînes mises bout à bout (le numéro
// d'itération saute entre deux chaînes) ; années calendaires signées (BC/AD) ; séparateur et décimale selon les
// réglages CSV de ChronoModel (« , » et « . », ou « ; » et « , »).
//
// Les tirages restent dans l'ordre : une quantité dérivée (B − A, date entre A et B) est calculée tirage par tirage,
// ce qui garde la dépendance entre Events (contraintes stratigraphiques, phases).
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>

namespace seldon {

struct TraceSet {
    QString     file;                         // nom du fichier
    QString     dateFormat;                   // ligne « # Date Format : … » de l'export (vide si absente)
    QString     csvFormat;                    // séparateur et décimale repérés (« séparateur « ; », décimale « , » »)
    QStringList columns;                      // noms des colonnes (sans « iter »)
    std::vector<std::vector<double>> values;  // values[colonne][tirage]
    std::vector<size_t> chainStarts;          // indice du premier tirage de chaque chaîne
    size_t rows() const { return values.empty() ? 0 : values.front().size(); }
};

namespace traces {

// Le texte ressemble à un export de chaînes de ChronoModel (première ligne « iter » + colonnes, valeurs numériques).
bool looksLike(const QString& text);
bool parse(const QString& text, const QString& fileName, TraceSet& out, QString* error = nullptr);

// Description pour le modèle de langage et la fiche : fichier, chaînes, tirages, colonnes.
QString summary(const TraceSet& s);

// Traces chargées dans la session (pièces jointes) : un fichier remplace celui de même nom.
void add(const TraceSet& s);
void clear();
const QVector<TraceSet>& loaded();

// Colonne désignée par un nom : nom exact (sans casse ni accents), ou « début de X » / « fin de X » / « X début » /
// « X fin » / « begin X » / « end X » pour « X Begin » / « X End ». `set` et `column` repèrent la colonne ; faux
// et `error` sinon (nom inconnu ou ambigu, avec la liste des colonnes).
// `preferred` : fichier à prendre en priorité quand le nom existe dans plusieurs (le même calcul exporté deux fois).
bool resolve(const QString& name, int& set, int& column, QString* error = nullptr, int preferred = -1);

// Fichier qui contient toutes ces colonnes (index dans loaded()), -1 s'il n'y en a pas.
int setWithAll(const QStringList& names);

// Colonne citée dans une question : nom exact (« N(500) », « US 12 »), ou borne de phase en toutes lettres (« début
// de la phase A » pour « Phase A Begin » / « A Begin »). `pos` : position dans la question passée par fold().
struct Mention { QString column; int pos = 0; };
// Colonnes des traces chargées citées dans la question, dans leur ordre d'apparition (sans doublon ni chevauchement :
// la citation la plus longue l'emporte, « unknown3 » plutôt que « unknown »).
QVector<Mention> mentions(const QString& question);

// Phases citées par leur nom (« la phase A », « Bronze final ») dont les traces ont un début et une fin (« X Begin »,
// « X End ») : nom de la phase tel qu'il précède « Begin » / « End », dans l'ordre d'apparition.
QStringList phasesIn(const QString& question);

} // namespace traces
} // namespace seldon
