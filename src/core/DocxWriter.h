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


// Écriture d'un document Word (.docx, Office Open XML) à partir d'un QTextDocument : export de la conversation avec ses
// fiches, ses tableaux et ses graphiques (05/11/2026). Qt n'a pas d'écrivain .docx (QTextDocumentWriter : HTML, ODF,
// Markdown, texte) ; l'écrivain ODF de Qt perd le contenu des cellules imbriquées et les fonds, que les fiches utilisent.
// Parcours du document : blocs → paragraphes (titres, listes, alignement, marges, fond), fragments → runs (gras,
// italique, souligné, barré, couleur, taille, police à chasse fixe, exposant, fond), images (ressources du document ou
// data:image/…;base64) → images intégrées, liens http(s) et mailto → liens hypertexte, tableaux (imbriqués compris) →
// tableaux Word (largeurs, fusions, fonds de cellule, bordures, marges intérieures). Empaquetage par QZipWriter (API
// privée de QtGui, comme le lecteur des classeurs dans Spreadsheet).
#pragma once

#include <QByteArray>
#include <QString>

class QTextDocument;

namespace seldon::docx {

struct Options {
    QString title;                              // propriétés du document (Fichier > Propriétés dans Word)
    QString author = QStringLiteral("SeldonH");
    QString bodyFont = QStringLiteral("Arial");          // polices présentes partout (Source Sans 3 n'est pas installée chez tous)
    QString monoFont = QStringLiteral("Courier New");
    double  bodyPt = 10.5;
};

// Contenu du fichier .docx (zip) ; vide en cas d'erreur (`error` renseigné).
QByteArray build(const QTextDocument& doc, const Options& options = Options(), QString* error = nullptr);
bool write(const QTextDocument& doc, const QString& path, const Options& options = Options(), QString* error = nullptr);

} // namespace seldon::docx
