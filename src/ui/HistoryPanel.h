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

// HistoryPanel : contenu du volet « Historique » (à droite de la fenêtre).
// Liste des conversations enregistrées, la plus récente en haut. Ne touche pas aux fichiers : il émet des signaux,
// c'est MainWindow qui ouvre, supprime et réaffiche.
#pragma once

#include <QAbstractButton>
#include <QString>
#include <QVector>
#include <QWidget>

#include "../core/ConversationStore.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace seldon {

// Fine bande verticale toujours visible au bord de la fenêtre : un clic replie ou déplie le volet voisin.
// `isChecked()` = volet déplié. Le texte est écrit à la verticale.
class CollapseStrip : public QAbstractButton {
    Q_OBJECT
public:
    explicit CollapseStrip(const QString& label, QWidget* parent = nullptr);
    QSize sizeHint() const override { return QSize(22, 120); }
    QSize minimumSizeHint() const override { return QSize(22, 60); }
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { hover_ = true; update(); }
    void leaveEvent(QEvent*) override { hover_ = false; update(); }
private:
    bool hover_ = false;
};

class HistoryPanel : public QWidget {
    Q_OBJECT
public:
    explicit HistoryPanel(QWidget* parent = nullptr);

    // Remplace le contenu. `currentId` : conversation ouverte (affichée en gras).
    void setItems(const QVector<ConversationInfo>& items, const QString& currentId);
    QString selectedId() const;            // vide si rien n'est sélectionné
    int     visibleCount() const;          // lignes affichées (après filtre)
    void    setBusy(bool busy);            // pendant une génération : ouvrir/supprimer la conversation en cours est refusé par MainWindow, on grise « Ouvrir »

signals:
    void openRequested(const QString& id);
    void deleteRequested(const QString& id);
    void deleteAllRequested();

private:
    void rebuild();
    void updateButtons();

    QVector<ConversationInfo> items_;
    QString       currentId_;
    bool          busy_ = false;
    QLineEdit*    filter_;
    QListWidget*  list_;
    QLabel*       empty_;
    QPushButton*  openBtn_;
    QPushButton*  delBtn_;
    QPushButton*  delAllBtn_;
};

} // namespace seldon
