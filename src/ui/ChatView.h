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
#include <QMimeData>
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
    void setSuggestions(int index, const QString& html);       // suites proposées (petit, gris), sous les liens ; vide = retirées
    // Raisonnement du modèle : affiché pendant qu'il s'écrit puis replié (lien seldon://thinking/<index> pour le déplier).
    void setShowThinking(bool on);
    void toggleThinking(int index);

    // Rappel d'une figure du panneau de droite : vignette cliquable (lien seldon://figure/<id>). setFigure remplace
    // l'image d'une figure déjà rappelée (datations ajoutées à la même figure).
    // La vignette rejoint la fiche de calcul qui la précède (même boîte) ; sinon (tracé demandé par le modèle, dessin du
    // modèle) elle est placée dans une boîte « Graphique SeldonH », partagée par les vignettes qui se suivent.
    void addFigure(int id, const QImage& img, const QString& caption);
    void setFigure(int id, const QImage& img);

    // Réponse du modèle : libellé « Modèle d'IA · <nom> (local / distant) » (nom raccourci).
    void setModelLabel(int index, const QString& label);
    // Bulle de réponse remplie par SeldonH sans le modèle (mode calculatrice) : simple note sous la fiche, sans la boîte
    // de l'IA. Retirée dès que le modèle écrit dans la bulle (« Demander un commentaire »).
    void setAppNote(int index, bool on);
    // Lien « ✎ Modifier » sous la dernière question (seldon://edit) ; désactivé pendant une réponse.
    void setEditEnabled(bool on);
    int  lastUserIndex() const;               // -1 s'il n'y a aucune question
    QString userText(int index) const;        // texte brut d'une question affichée
    void truncate(int from);                  // retire les éléments à partir de `from` (modifier la dernière question)
    int  count() const { return int(items_.size()); }
    // Copie d'une fiche « Calcul SeldonH » (ou d'une boîte de graphiques) telle qu'affichée, en thème clair : HTML (images
    // intégrées, police précisée) et texte brut. Lien « Copier » de la fiche.
    bool copyCard(int index) const;
    QMimeData* cardMimeData(int index) const;     // (exposé pour les tests ; nullptr si ce n'est pas une fiche)
    static QString shortModelName(const QString& name, int maxChars = 32);   // « Mistral-Small-3.2-24B-Inst… »

    void clearAll();
    QString exportHtml() const;                // conversation complète (pour sauvegarde), thème clair, figures intégrées
    // Conversation complète en document Word (.docx) : fiches, tableaux et graphiques compris, thème clair, sans les liens
    // d'action (Copier, Relancer…). `title` : titre du document (propriétés).
    bool exportDocx(const QString& path, const QString& title, QString* error = nullptr) const;
    QTextDocument* exportDocument(QObject* parent) const;   // document Qt de l'export (exposé pour les tests)

    static QString markdownToHtml(const QString& md);

protected:
    QVariant loadResource(int type, const QUrl& name) override;    // images « seldon-fig:<id> »

private:
    struct Item {
        enum Kind { User, Assistant, Notice, Figure } kind = Notice;
        QString html;          // contenu rendu
        QString actions;       // liens sous la bulle
        QString suggestions;   // suites proposées (affichage seulement, pas dans l'export)
        QString thinking;
        bool    thinkingOpen = false;
        bool    final = false;           // réponse terminée (le raisonnement est alors replié)
        bool    thinkingShown = false;   // raisonnement déplié par l'utilisateur (lien « ▸ Raisonnement »)
        int     figure = -1;   // Figure : identifiant de la première image
        QVector<int> figs;     // vignettes de la boîte (fiche de calcul ou boîte « Graphique »)
        QString plain;         // User : texte brut de la question
        QString model;         // Assistant : modèle qui a répondu (libellé déjà raccourci)
        bool    appNote = false;    // Assistant : note de SeldonH (pas de réponse du modèle)
    };
    struct FigureImage { QImage img; int version = 0; QString caption; };
    void scheduleRender();
    void render();
    QString bubble(const Item& it, bool forExport = false, bool last = false, bool firstAi = false) const;
    QString figuresHtml(const Item& it, bool forExport, const QString& muted) const;

    QVector<Item> items_;
    QHash<int, FigureImage> figures_;
    QTimer        timer_;
    bool          dirty_ = false;
    bool          showThinking_ = false;
    bool          editEnabled_ = true;
    mutable bool  forceLight_ = false;     // copie d'une fiche : couleurs du thème clair
    mutable bool  plainFigUrls_ = false;   // copie d'une fiche : images nommées « fig_<id>.png » (ressources du document)
};

} // namespace seldon
