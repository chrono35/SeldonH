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
