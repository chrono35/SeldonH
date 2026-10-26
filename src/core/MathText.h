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

// MathText : affichage des formules LaTeX dans le dialogue.
//
// Qt n'a pas de moteur LaTeX : sans traitement, une réponse comme « $\sigma = \sqrt{\sigma_1^2 + \sigma_2^2}$ » s'affiche
// avec ses signes de construction (\sigma, \sqrt, {, }, ^, _). Ce module convertit les formules courantes en HTML
// compatible QTextDocument (lettres grecques et symboles Unicode, <sup>/<sub>, racines surlignées, fractions, fonctions
// droites…). Ce n'est pas TeX : les constructions très complexes (matrices, alignements) sont seulement mises en lignes.
#pragma once

#include <QString>
#include <QStringList>

namespace seldon {
namespace math {

// Fragment HTML d'une formule LaTeX (sans les délimiteurs $…$). `display` : formule hors texte (centrée par restore()).
QString latexToHtml(const QString& latex);

// Remplace dans un texte Markdown les formules ($…$, $$…$$, \(…\), \[…\] et quelques commandes isolées comme \frac)
// par des marqueurs neutres pour Markdown, hors blocs et codes en ligne. `rendered` reçoit le HTML de chaque formule.
QString protect(const QString& markdown, QStringList& rendered);

// Remet le HTML des formules à la place des marqueurs dans le HTML produit par Markdown.
QString restore(QString html, const QStringList& rendered);

} // namespace math
} // namespace seldon
