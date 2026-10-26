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

#include "HistoryPanel.h"

#include <QAction>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace seldon {

CollapseStrip::CollapseStrip(const QString& label, QWidget* parent) : QAbstractButton(parent)
{
    setText(label);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setFocusPolicy(Qt::TabFocus);
}

void CollapseStrip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette& pal = palette();
    p.fillRect(rect(), hover_ ? pal.color(QPalette::Midlight) : pal.color(QPalette::Window));
    p.setPen(pal.color(QPalette::Mid));
    p.drawLine(width() - 1, 0, width() - 1, height());
    if (isChecked()) p.drawLine(0, 0, 0, height());      // volet ouvert : la bande le borde à droite

    p.setPen(pal.color(QPalette::ButtonText));
    // Flèche en haut : ◀ quand le volet est ouvert (cliquer le replie), ▶ quand il est replié.
    const QString arrow = isChecked() ? QStringLiteral("◀") : QStringLiteral("▶");
    p.drawText(QRect(0, 6, width() - 1, 20), Qt::AlignCenter, arrow);
    // Texte à la verticale, lisible de bas en haut.
    p.save();
    p.translate(0, height());
    p.rotate(-90);
    p.drawText(QRect(10, 0, height() - 40, width() - 1), Qt::AlignLeft | Qt::AlignVCenter, text());
    p.restore();
}

HistoryPanel::HistoryPanel(QWidget* parent) : QWidget(parent)
{
    newBtn_ = new QPushButton(tr("＋ Nouvelle conversation"));
    newBtn_->setToolTip(tr("Commence une conversation vide (la conversation actuelle reste dans l'historique)"));

    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Rechercher dans les titres…"));
    filter_->setClearButtonEnabled(true);

    list_ = new QListWidget;
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    list_->setUniformItemSizes(false);
    list_->setWordWrap(true);
    list_->setAlternatingRowColors(true);
    list_->setTextElideMode(Qt::ElideRight);

    empty_ = new QLabel;
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setEnabled(false);

    openBtn_ = new QPushButton(tr("Ouvrir"));
    delBtn_ = new QPushButton(tr("Supprimer"));
    delAllBtn_ = new QPushButton(tr("Tout effacer…"));
    delAllBtn_->setToolTip(tr("Supprime toutes les conversations enregistrées sur cet ordinateur"));

    auto* row = new QHBoxLayout;
    row->addWidget(openBtn_);
    row->addWidget(delBtn_);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->addWidget(newBtn_);
    lay->addWidget(filter_);
    lay->addWidget(list_, 1);
    lay->addWidget(empty_, 1);
    lay->addLayout(row);
    lay->addWidget(delAllBtn_);

    connect(newBtn_, &QPushButton::clicked, this, &HistoryPanel::newRequested);
    connect(filter_, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(list_, &QListWidget::itemSelectionChanged, this, &HistoryPanel::updateButtons);
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        if (it && !busy_) emit openRequested(it->data(Qt::UserRole).toString());
    });
    connect(openBtn_, &QPushButton::clicked, this, [this] {
        const QString id = selectedId();
        if (!id.isEmpty()) emit openRequested(id);
    });
    auto askDelete = [this] {
        const QString id = selectedId();
        if (!id.isEmpty()) emit deleteRequested(id);
    };
    connect(delBtn_, &QPushButton::clicked, this, askDelete);
    for (const Qt::Key k : { Qt::Key_Delete, Qt::Key_Backspace }) {
        auto* sc = new QShortcut(QKeySequence(k), list_, nullptr, nullptr, Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, askDelete);
    }
    connect(delAllBtn_, &QPushButton::clicked, this, &HistoryPanel::deleteAllRequested);
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = list_->itemAt(pos);
        if (!it) return;
        list_->setCurrentItem(it);
        const QString id = it->data(Qt::UserRole).toString();
        QMenu menu(this);
        QAction* open = menu.addAction(tr("Ouvrir"));
        open->setEnabled(!busy_);
        QAction* del = menu.addAction(tr("Supprimer"));
        QAction* chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
        if (chosen == open) emit openRequested(id);
        else if (chosen == del) emit deleteRequested(id);
    });
    rebuild();
}

void HistoryPanel::setItems(const QVector<ConversationInfo>& items, const QString& currentId)
{
    items_ = items;
    currentId_ = currentId;
    rebuild();
}

QString HistoryPanel::selectedId() const
{
    const auto sel = list_->selectedItems();
    return sel.isEmpty() ? QString() : sel.first()->data(Qt::UserRole).toString();
}

int HistoryPanel::visibleCount() const { return list_->count(); }

void HistoryPanel::setBusy(bool busy)
{
    busy_ = busy;
    updateButtons();
}

void HistoryPanel::rebuild()
{
    const QString keepSelected = selectedId();
    const QString needle = filter_->text().trimmed();
    list_->clear();
    for (const ConversationInfo& c : items_) {
        if (!needle.isEmpty() && !c.title.contains(needle, Qt::CaseInsensitive)) continue;
        auto* it = new QListWidgetItem(QStringLiteral("%1\n%2 · %3 %4")
                                           .arg(c.title, QLocale().toString(c.updated, QStringLiteral("d MMM yyyy HH:mm")))
                                           .arg(c.exchanges).arg(c.exchanges > 1 ? tr("échanges") : tr("échange")));
        it->setData(Qt::UserRole, c.id);
        it->setToolTip(c.backend.isEmpty() ? c.title : c.title + "\n" + c.backend);
        if (c.id == currentId_) {
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
        }
        list_->addItem(it);
        if (c.id == keepSelected) list_->setCurrentItem(it);
    }
    const bool none = list_->count() == 0;
    list_->setVisible(!none);
    empty_->setVisible(none);
    empty_->setText(items_.isEmpty() ? tr("Aucune conversation enregistrée.\nLes échanges sont conservés automatiquement.")
                                     : tr("Aucun titre ne correspond."));
    delAllBtn_->setEnabled(!items_.isEmpty());
    updateButtons();
}

void HistoryPanel::updateButtons()
{
    const bool has = !selectedId().isEmpty();
    openBtn_->setEnabled(has && !busy_);
    delBtn_->setEnabled(has);
}

} // namespace seldon
