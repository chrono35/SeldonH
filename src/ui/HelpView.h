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

// HelpView : affiche le mode d'emploi (resources/help/aide.html) dans le panneau de droite.
//
// Le fichier HTML est la source unique : il s'ouvre tel quel dans un navigateur, et ici on en retire <style> et
// <script> pour appliquer une mise en forme adaptée à QTextBrowser (couleurs du thème système). Les exemples
// cliquables s'écrivent <a class="ex" data-method="c14">texte</a> ; l'application leur ajoute un lien
// seldon://insert?q=<texte>, qui recopie la phrase dans la zone de question.
#pragma once

#include <QList>
#include <QString>
#include <QTextBrowser>

namespace seldon {

class HelpView : public QTextBrowser {
    Q_OBJECT
public:
    struct Example {
        QString text;      // phrase à poser
        QString method;    // méthode de calcul attendue (data-method) ; vide si pas de calcul
    };

    explicit HelpView(QWidget* parent = nullptr);

    static QString sourceHtml();                              // contenu de :/help/aide.html
    static QList<Example> examples(const QString& html);      // tous les <a class="ex">
    static QString toQtHtml(const QString& html);             // sans <style>/<script>, avec les liens des exemples
    static QString styleSheet(const QPalette& pal);           // feuille de style par défaut du document
    static QString exportForBrowser();                        // écrit une copie dans un dossier temporaire ; renvoie son chemin ("" si échec)

signals:
    void insertRequested(const QString& text);

private slots:
    void onAnchor(const QUrl& url);
};

} // namespace seldon
