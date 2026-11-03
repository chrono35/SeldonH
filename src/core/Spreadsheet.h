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

// Spreadsheet : lecture d'une feuille de classeur .xlsx (Excel, Office Open XML) ou .ods (LibreOffice, OpenDocument).
//
// Les deux formats sont des archives zip de fichiers XML ; elles sont ouvertes avec le lecteur zip de Qt (QZipReader,
// module « gui-private ») et lues avec QXmlStreamReader : ni Excel, ni réseau, ni bibliothèque de plus.
// Rendu : une grille de textes (une cellule = un texte, nombres écrits sans perte), avec le numéro de ligne de la feuille.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {
namespace spreadsheet {

struct Sheet {
    QString              name;
    QVector<QStringList> rows;          // lignes non vides, cellules vides replacées (colonnes alignées)
    QVector<int>         lineNumbers;   // numéro de chaque ligne dans la feuille (1 = première)
    QStringList          notes;         // cellules au format date, formules sans valeur, feuille tronquée…
};

bool isSpreadsheet(const QString& path);                         // .xlsx, .xlsm, .ods
QStringList sheetNames(const QString& path, QString* error = nullptr);
// Lit la feuille `sheet` (vide = la première). Faux et `error` si le fichier est illisible.
bool read(const QString& path, const QString& sheet, Sheet& out, QString* error = nullptr);

// Index de colonne d'une référence Excel : « B12 » → 1, « AA3 » → 26 ; numéro de ligne : 12, 3.
int columnIndex(const QString& cellRef);
int rowNumber(const QString& cellRef);

} // namespace spreadsheet
} // namespace seldon
