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

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QVector>

#include <QPointer>

#include "src/core/AppSettings.h"
#include "src/core/Calculator.h"
#include "src/core/ChronoResults.h"
#include "src/core/ConversationStore.h"
#include "src/llm/LlmEngine.h"
#include "src/llm/RemoteApi.h"
#include "src/methods/common/DateFormat.h"
#include "src/skills/Lexicon.h"
#include "src/skills/SkillManager.h"

class QAction;
class QLabel;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;
class QSplitter;
class QThread;
class QToolButton;

namespace seldon {
class RemoteEngine;
class ChatView;
class FigurePanel;
class ChrSceneWidget;
class HelpView;
class HistoryPanel;
class CollapseStrip;
class InputEdit;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    // Appels vers le thread du moteur (liaisons en file d'attente).
    void requestLoad(const QString& path, int nCtx, int nThreads, int nGpuLayers);
    void requestGenerate(const QVector<seldon::ChatMessage>& chat, const seldon::SamplingParams& sp);
    void requestReset();
    void requestRemoteGenerate(const QVector<seldon::ChatMessage>& chat, const seldon::SamplingParams& sp);

private slots:
    void sendMessage();
    void stopGeneration();
    void newConversation();
    void chooseModel();
    void openPreferences();
    void attachFile();
    void clearAttachment();
    void saveTranscript();
    void openSkillsFolder();
    void openModelsFolder();
    void reloadSkills();
    void openCurvesFolder();
    void reloadCurves();
    void showCurves();
    void saveDensityFile(const QUrl& url);
    QVector<QJsonObject> lastDatingRequests() const;
    void showAbout();
    void showHelp();
    void openHelpInBrowser();
    void insertExample(const QString& text);

    void onModelLoaded(const QString& description, int ctx);
    void onModelFailed(const QString& error);
    void onToken(const QString& piece);
    void onFinished(const seldon::GenStats& st);
    void onFailed(const QString& error);
    void onLink(const QUrl& url);
    void onToolCalls(const QVector<seldon::ToolCall>& calls, const QString& text, const seldon::GenStats& st);

    // Historique des conversations (volet de droite)
    void saveCurrent();                         // enregistre la conversation affichée (si l'historique est activé)
    void openConversation(const QString& id);
    void deleteConversation(const QString& id);
    void deleteAllConversations();
    void setHistoryVisible(bool on);

private:
    struct Turn {
        QString     question;      // ce que voit l'utilisateur
        QString     modelUser;     // ce que voit le modèle (question + connaissances + pièce jointe)
        QString     answer;        // réponse finale (sans réflexion)
        QString     toolBlocks;    // appels d'outils natifs de ce tour, ré-écrits en blocs ```calcul pour l'historique
        QStringList skillIds;      // compétences injectées dans ce tour
        int         view = -1;     // index de la bulle dans ChatView
        bool        synthetic = false;  // tour créé par l'application après un calcul (pas une question de l'utilisateur)
        bool        followUp = false;   // vrai : pas (ou plus) de calcul à relancer pour ce tour (déjà calculé par l'application, ou tour de suite)
        QString     thinking;           // réflexion affichée (gardée quand la réponse est continuée)
        int         truncation = 0;     // GenStats::Truncation de la dernière génération (réponse coupée : bouton « Continuer »)
        int         tokenLimit = 0;
        QString     truncationDetail;
        bool        drewModel = false;  // SeldonH a dessiné le modèle pour ce tour (bloc plot du modèle de langage ignoré)
        QJsonObject prompt;             // dernier prompt envoyé pour ce tour (voir promptSnapshot)
    };

    void buildUi();
    void buildMenus();
    void loadModel(const QString& path);
    void setBusy(bool generating);
    void updateControls();
    void showWelcome();
    void finishAssistantMessage(bool interrupted);
    int  recallFigure(const QJsonObject& plot);   // vignette dans le dialogue de la courbe ajoutée en dernier au panneau
    void showFigure(int id);                      // clic sur une vignette : onglet du panneau (recréé s'il a été fermé)
    void showFiguresPanel();                      // ouvre le panneau des graphiques (masqué, ou replié avec le séparateur)
    void continueAnswer(int turn);              // reprend une réponse coupée (limite de tokens, contexte, flux interrompu)
    QString truncationNotice(const Turn& t) const;
    void rememberChrDates(const QJsonObject& chr);
    static QString historyUser(const Turn& t);          // message d'un tour passé, sans ses fiches de connaissances
    QString shownAnswer(const QString& answer) const;   // bloc de modèle remplacé par un résumé (JSON seulement sur demande)
    bool modelExpected() const;                           // la conversation demande un modèle ChronoModel (bloc chronomodel admis)
    // Messages envoyés au modèle. `refs` (facultatif) : les mêmes messages sous forme compacte, pour l'instantané du prompt.
    QVector<seldon::ChatMessage> buildChat(QJsonArray* refs = nullptr) const;
    // Instantané de ce qui est envoyé au modèle : date, version, moteur, paramètres d'échantillonnage, messages (renvois).
    QJsonObject promptSnapshot(const QJsonArray& messages, const seldon::SamplingParams& sp) const;
    // Messages complets d'un instantané (renvois remplacés par le texte des tours).
    QJsonArray expandPromptMessages(const QJsonArray& messages) const;
    void relaunchTurn(int turn);          // « ↻ Relancer » : redemande la question de ce tour, avec le même prompt
    void exportPrompt(int turn);          // « Exporter le prompt » : JSON (reproductibilité) ou Markdown (lecture)
    // Ce qu'a donné l'utilisateur dans la conversation : ses questions et le texte de ses fichiers joints (vérification
    // croisée d'un modèle écrit par le modèle de langage).
    QString userSourceText() const;
    void saveChrFromTurn(int turn);
    // Dessine un .chr dans le panneau : scène des Events et, s'il a des phases, scène des phases. `inChat` : avis et
    // vignettes cliquables ajoutés à la discussion (et à l'historique).
    void showChrScenes(const QJsonObject& chr, const QString& name, bool inChat);
    // Ouvre (ou met à jour) l'onglet d'une scène ; `kind` : « events » ou « phases ».
    void showChrScene(const QJsonObject& chr, const QString& kind, const QString& title);
    // Vignette cliquable d'une scène dans la discussion (`fig` : {"chr_scene", "chr", "title"}).
    void addSceneFigure(const QJsonObject& fig);
    void applyPlotBlocks(const QString& answer);
    void activateRemote();                     // configure et active le modèle distant (Préférences)
    bool remoteActive() const { return settings_.useRemote; }
    void startGeneration();                    // lance la génération pour les tours en cours
    seldon::DateFormat displayFormat() const;  // Préférences, ou format demandé dans la dernière vraie question
    bool runRequestedCalculations();           // blocs ```calcul de la dernière réponse -> calcul + tour de suite

    // Tout ce qui s'affiche passe par ces fonctions : elles gardent aussi le journal d'affichage (display_) qui sert à
    // redessiner la conversation quand on la rouvre depuis l'historique.
    void logUser(const QString& shown);
    void logNotice(const QString& html);
    bool logPlot(const QJsonObject& plot, QString* err = nullptr);   // false (et err) si le tracé est inexploitable
    // Courbes d'une datation calculée : toutes les datations d'une même question vont dans UNE figure (superposées),
    // même si elles arrivent en plusieurs fois (calcul automatique, puis bloc « calcul » ou outils de l'assistant).
    void logDatingPlot(const QJsonObject& plot, int questionTurn);
    int  currentQuestionTurn() const;            // dernier tour posé par l'utilisateur (non synthétique)
    int  newAssistantBubble();                 // bulle du prochain tour (index = turns_.size())
    void logAssistantText(int turn, const QString& markdown, const QString& thinking);
    void setTurnActions(int turn);             // liens « Enregistrer le modèle » / « Copier » sous la réponse
    void resetConversation(bool clearFigures);  // vide l'écran et l'état, sans rien enregistrer ni effacer
    void refreshHistory();                     // relit tout le dossier (au démarrage, après suppression)
    void touchHistoryEntry(const seldon::Conversation& c);   // met à jour la liste sans relire les fichiers

    seldon::AppSettings  settings_;
    seldon::SkillManager skills_;
    seldon::Lexicon      lexicon_;                        // repérage des mots-clés en en, es, it, de

    QThread*             thread_ = nullptr;
    seldon::LlmEngine*   engine_ = nullptr;
    seldon::RemoteEngine* remote_ = nullptr;     // modèle distant (API compatible OpenAI), dans le thread de l'interface

    seldon::ChatView*    chat_ = nullptr;
    seldon::FigurePanel* figures_ = nullptr;
    seldon::HistoryPanel* history_ = nullptr;
    seldon::CollapseStrip* historyStrip_ = nullptr;   // bande toujours visible (à droite du volet ouvert) : replie/déplie l'historique
    QAction*             historyAction_ = nullptr;
    QSplitter*           outer_ = nullptr;            // [historique | bande | (dialogue | graphiques)]
    QPointer<seldon::HelpView> help_;       // onglet d'aide (supprimé si l'utilisateur ferme l'onglet)
    QAction*             figAction_ = nullptr;
    seldon::InputEdit*   input_ = nullptr;
    QPushButton*         sendBtn_ = nullptr;
    QPushButton*         stopBtn_ = nullptr;
    QToolButton*         attachBtn_ = nullptr;
    QLabel*              attachLabel_ = nullptr;
    QToolButton*         attachClear_ = nullptr;
    QLabel*              modelLabel_ = nullptr;
    QLabel*              statsLabel_ = nullptr;
    QLabel*              quotaLabel_ = nullptr;     // à droite : place restante dans le contexte, quota du fournisseur
    int                  localCtx_ = 0;             // contexte du modèle local chargé (tokens)
    int                  remoteCtx_ = 0;            // contexte du modèle distant (liste /models), 0 = inconnu
    QNetworkAccessManager* limitsNam_ = nullptr;    // GET /models pour connaître ce contexte
    void updateQuota(const seldon::GenStats& st);
    void resetQuota();
    void fetchRemoteContext();
    QProgressBar*        loadBar_ = nullptr;
    QSplitter*           splitter_ = nullptr;
    QSplitter*           inputSplitter_ = nullptr;      // [conversation | saisie], saisie redimensionnable
    QToolButton*         newConvBtn_ = nullptr;         // barre de titre : nouvelle conversation
    QLabel*              convTitle_ = nullptr;          // barre de titre : titre de la conversation en cours
    void updateConvTitle();
    seldon::ConversationStore store_{ seldon::ConversationStore::defaultDir() };
    QVector<seldon::ConversationInfo> historyItems_;
    QString              convId_;              // vide tant que rien n'a été demandé
    QDateTime            convCreated_;
    QVector<seldon::DisplayItem> display_;     // journal d'affichage de la conversation
    int                  datingPlotTurn_ = -1;     // question dont la figure de datations est datingPlotPage_
    int                  datingPlotItem_ = -1;     // son entrée dans display_
    QPointer<QWidget>    datingPlotPage_;
    int                  datingPlotFig_ = -1;      // son rappel dans le dialogue (indice de chatFigures_)
    // Figures rappelées dans le dialogue (vignettes) : JSON de la courbe et onglet du panneau de droite (nul si fermé).
    struct ChatFigure { QJsonObject plot; QPointer<QWidget> page; };
    QVector<ChatFigure>  chatFigures_;

    QVector<Turn>        turns_;
    QString              raw_;                 // sortie brute du modèle pour le tour en cours
    bool                 startsInThink_ = false;
    bool                 modelReady_ = false;
    bool                 loading_ = false;
    bool                 generating_ = false;
    QString              attachName_, attachText_;
    bool                 attachChronoResults_ = false;   // pièce jointe = résultats de ChronoModel (résumé extrait)
    seldon::chronoresults::Report attachResults_;         // résultats joints, lus
    seldon::chronoresults::Report lastResults_;           // derniers résultats de la conversation (questions suivantes)
    QString              lastResultsName_;
    seldon::Calculator   calc_;                           // calculatrice locale (variables de la conversation)
    QString calcHtml(const QVector<seldon::Calculator::Line>& lines) const;
    QJsonObject          attachChr_;                      // pièce jointe = projet .chr (dessinable)
    QJsonObject          lastChr_;                        // dernier .chr de la conversation (joint ou créé)
    QVector<QJsonObject> chrDates_;                       // dates des .chr joints dans la conversation : noms repris dans les calculs
    QString              lastChrTitle_;
    QPointer<seldon::ChrSceneWidget> eventScene_, phaseScene_;   // onglets des scènes (nuls si fermés)
    QString              modelDesc_;
    seldon::ModelInfo    modelInfo_;            // modèle local chargé (pour « Optimiser pour ce modèle »)
    double               lastLocalTps_ = 0;     // vitesse de génération mesurée sur la dernière réponse locale
    bool                 continuing_ = false;   // génération en cours = suite d'une réponse coupée
};

#endif // MAINWINDOW_H
