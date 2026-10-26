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

// ChatView : zone de réponses. Rend la conversation en HTML (Markdown -> HTML via Qt).
#pragma once

#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QTextBrowser>
#include <QTimer>
#include <QVector>

namespace seldon {

class ChatView : public QTextBrowser {
    Q_OBJECT
public:
    explicit ChatView(QWidget* parent = nullptr);

    int  addUser(const QString& text);
    int  addAssistant();                       // crée une bulle vide et retourne son index
    void addNotice(const QString& html);       // message système (HTML brut)

    // Met à jour une bulle de réponse. `final` : rendu définitif (sinon rendu throttlé).
    void setAssistant(int index, const QString& markdown, const QString& thinking,
                      bool thinkingOpen, bool final);
    void setActions(int index, const QStringList& linksHtml);   // liens sous la bulle

    // Rappel d'une figure du panneau de droite : vignette cliquable (lien seldon://figure/<id>). setFigure remplace
    // l'image d'une figure déjà rappelée (datations ajoutées à la même figure).
    void addFigure(int id, const QImage& img, const QString& caption);
    void setFigure(int id, const QImage& img);

    void clearAll();
    QString exportHtml() const;                // conversation complète (pour sauvegarde)

    static QString markdownToHtml(const QString& md);

protected:
    QVariant loadResource(int type, const QUrl& name) override;    // images « seldon-fig:<id> »

private:
    struct Item {
        enum Kind { User, Assistant, Notice, Figure } kind = Notice;
        QString html;          // contenu rendu
        QString actions;       // liens sous la bulle
        QString thinking;
        bool    thinkingOpen = false;
        int     figure = -1;   // Figure : identifiant de l'image
    };
    struct FigureImage { QImage img; int version = 0; QString caption; };
    void scheduleRender();
    void render();
    QString bubble(const Item& it, bool forExport = false) const;

    QVector<Item> items_;
    QHash<int, FigureImage> figures_;
    QTimer        timer_;
    bool          dirty_ = false;
};

} // namespace seldon
