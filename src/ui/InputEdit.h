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
