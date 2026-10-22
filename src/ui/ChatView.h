// ChatView : zone de réponses. Rend la conversation en HTML (Markdown -> HTML via Qt).
#pragma once

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

    void clearAll();
    QString exportHtml() const;                // conversation complète (pour sauvegarde)

    static QString markdownToHtml(const QString& md);

private:
    struct Item {
        enum Kind { User, Assistant, Notice } kind = Notice;
        QString html;          // contenu rendu
        QString actions;       // liens sous la bulle
        QString thinking;
        bool    thinkingOpen = false;
    };
    void scheduleRender();
    void render();
    QString bubble(const Item& it) const;

    QVector<Item> items_;
    QTimer        timer_;
    bool          dirty_ = false;
};

} // namespace seldon
