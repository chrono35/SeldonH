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

// Zone de saisie : Entrée envoie, Maj+Entrée insère un retour à la ligne.
#pragma once

#include <QKeyEvent>
#include <QPlainTextEdit>

namespace seldon {

class InputEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit InputEdit(QWidget* parent = nullptr) : QPlainTextEdit(parent)
    {
        setPlaceholderText(tr("Posez votre question… (Entrée pour envoyer, Maj+Entrée pour une nouvelle ligne)"));
        setTabChangesFocus(true);
    }
    QSize sizeHint() const override { return QSize(400, fontMetrics().lineSpacing() * 3 + 16); }

signals:
    void submitted();

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier)) {
            emit submitted();
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }
};

} // namespace seldon
