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
