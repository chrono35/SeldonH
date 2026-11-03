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

// DataTable : tableaux de datations (texte collé, CSV, TSV, Markdown, feuille .xlsx / .ods déjà lue).
//
// SeldonH lit le tableau lui-même (en-têtes français ou anglais, méthode donnée par une colonne, ou déduite des colonnes,
// de l'unité « BP » ou du code de laboratoire) et garde le « tableau courant » de la conversation. Le modèle de langage
// n'en reçoit qu'un résumé et demande des SÉLECTIONS (méthode, lignes, noms, valeurs d'une colonne) avec une action
// (extraire, dater, événement, combinaison, modèle ChronoModel) : les valeurs viennent toujours du tableau, jamais du
// texte du modèle. QtCore seulement (utilisé aussi par seldonh-mcp).
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {
namespace datatable {

// Rôle reconnu d'une colonne d'après son en-tête.
enum class Role { Other, Name, Method, Age, Error, RefYear, Curve, DeltaR, DeltaRError, F14C, Inc, Dec, Alpha95, Field, ErrorF,
                  Min, Max, Wiggle, Color };
QString roleTitle(Role r);

struct Table {
    QString            source;       // nom du fichier, ou « tableau collé »
    QString            sheet;        // feuille (.xlsx / .ods)
    QStringList        headers;      // en-têtes tels qu'écrits
    QVector<Role>      roles;        // rôle de chaque colonne
    QVector<QStringList> cells;      // lignes de données (après l'en-tête), alignées sur headers
    QVector<int>       sourceLines;  // numéro de chaque ligne dans le fichier ou la feuille (1 = première ligne)
    QVector<QJsonObject> requests;   // datation de chaque ligne ({"method":"c14","age":…}), vide si la ligne n'est pas datable
    QStringList        problems;     // raison pour une ligne non datable (vide sinon)
    QStringList        notes;        // remarques de lecture (méthode déduite, dates Excel, lignes ignorées…)
    int  headerLine = 0;             // ligne de l'en-tête dans la source
    bool isValid() const { return !headers.isEmpty() && !cells.isEmpty(); }
    int  datable() const;            // nombre de lignes datables
    int  column(const QString& header) const;      // colonne d'après son en-tête (sans casse ni accents), -1 sinon
    QString title() const;           // « fichier.xlsx — feuille Datations », « tableau collé »
};

// Grille de cellules → tableau : repère la ligne d'en-tête (dans les 15 premières lignes), les rôles des colonnes et la
// datation de chaque ligne. `error` : raison si aucun en-tête de tableau de datations n'est reconnu.
Table fromGrid(const QVector<QStringList>& grid, const QString& source, QString* error = nullptr, const QVector<int>& lineNumbers = {});

// Texte (tabulations, « ; », « | » ou Markdown, « , » en dernier recours) → grille, puis tableau.
QVector<QStringList> splitText(const QString& text);
Table parseText(const QString& text, const QString& source, QString* error = nullptr);

// Le texte contient-il un tableau (au moins deux lignes de 3 cellules ou plus, même séparateur) ? `start`/`length` :
// passage du tableau dans le texte (le reste est la consigne de l'utilisateur).
bool looksLikeTable(const QString& text, int* start = nullptr, int* length = nullptr);

// Ressemble au format d'import de ChronoModel (« method;dating name;… ») ou aux traces MCMC : pas un tableau à lire ici.
bool isChronoModelImport(const QString& text);

// Tableau courant de la conversation (comme les traces MCMC).
void setCurrent(const Table& t);
void clear();
bool hasCurrent();
const Table& current();

// Sélection de lignes. Spécification (outil « tableau », bloc calcul {"method":"table",…}) :
//   methods       : ["c14","tl"]          lignes de ces méthodes
//   rows          : [2,5,7]               numéros de ligne (colonne « # » de SeldonH, à partir de 1)
//   exclude_rows  : [9]
//   names         : ["Ly-1234"]           noms (colonne de nom), sans casse
//   exclude_names : […]
//   where         : [{"column":"US","op":"=","value":"12"}, …]   op : =, !=, contains, not_contains, <, >, <=, >=, in
//                   (column « méthode », « nom », « âge », « erreur » acceptées même si l'en-tête diffère)
// Sans critère : toutes les lignes. Les lignes non datables sont gardées pour « extract » et écartées des calculs.
struct Selection {
    QVector<int> rows;       // index dans Table::cells
    QString      criteria;   // en clair : « méthode 14C ; US = 12 »
    QString      error;      // spécification incomprise (colonne inconnue…)
};
Selection select(const Table& t, const QJsonObject& spec);

// Datations des lignes choisies (lignes non datables ignorées) ; `skipped` : « ligne 4 : erreur manquante ».
QVector<QJsonObject> requestsOf(const Table& t, const QVector<int>& rows, QStringList* skipped = nullptr);

// Rendu : tableau HTML (colonne # et méthode reconnue), CSV (séparateur « ; », guillemets si besoin).
QString rowsHtml(const Table& t, const QVector<int>& rows, int maxRows = 200);
QString toCsv(const Table& t, const QVector<int>& rows);

// Fiche « tableau compris » (pour l'utilisateur) et résumé pour le modèle de langage (budget en caractères).
QString summaryHtml(const Table& t);
QString digest(const Table& t, int budget);

// Spécification de modèle ChronoModel (format du bloc chronomodel de SeldonH, lu par ChrBuilder) : un événement par
// ligne, ou un événement par valeur de la colonne `groupBy` (« US »), nommé d'après cette valeur.
QJsonObject modelSpec(const Table& t, const QVector<int>& rows, const QString& groupBy, const QString& name, QString* error = nullptr);

// Demande directe, sans modèle de langage : « extrais les 14C du tableau », « date les TL du tableau », « calibre toutes
// les dates du tableau », « fais un événement avec les 14C du tableau ». Vide si la question n'en est pas une, ou si elle
// contient d'autres critères (« sauf », « de l'US 12 », « sur os »…) que seul le modèle peut traduire en sélection.
QJsonObject requestFromQuestion(const QString& question);

} // namespace datatable
} // namespace seldon
