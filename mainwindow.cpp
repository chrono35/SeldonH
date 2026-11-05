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

#include "mainwindow.h"

#include <QNetworkRequest>

#include <QNetworkReply>

#include <QNetworkAccessManager>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QToolButton>
#include <QSvgRenderer>
#include <QPainter>
#include <QTextCursor>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <QJsonObject>

#include "src/core/ChatText.h"
#include "src/core/ChrBuilder.h"
#include "src/core/ChronoResults.h"
#include "src/methods/common/CurveStore.h"
#include "src/methods/common/DensityFile.h"
#include "src/methods/common/MethodRunner.h"
#include "src/methods/common/MethodRegistry.h"
#include "src/methods/common/TextUtil.h"
#include "src/methods/common/Tools.h"
#include "src/methods/common/CurveTraces.h"
#include "src/methods/common/Traces.h"
#include "src/methods/common/DataTable.h"
#include "src/core/Spreadsheet.h"
#include "src/core/Suggestions.h"
#include "src/llm/RemoteEngine.h"
#include "src/llm/RemoteApi.h"
#include "src/ui/ChatView.h"
#include "src/ui/ChrScene.h"
#include "src/ui/FigurePanel.h"
#include "src/ui/HelpView.h"
#include "src/ui/HistoryPanel.h"
#include "src/ui/InputEdit.h"
#include "src/ui/SettingsDialog.h"

using namespace seldon;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("SeldonH"));
    resize(1280, 800);

    settings_ = AppSettings::load();
    dateformat::setDecimals(settings_.decimals);
    skills_.reload(settings_.skillsDir);
    lexicon_.reload(settings_.skillsDir);
    CurveStore::setExtraRoots(settings_.curveRoots());

    buildUi();
    buildMenus();

    // --- Moteur IA dans son propre thread ---
    thread_ = new QThread(this);
    engine_ = new LlmEngine;
    engine_->moveToThread(thread_);
    connect(thread_, &QThread::finished, engine_, &QObject::deleteLater);

    connect(this, &MainWindow::requestLoad, engine_, &LlmEngine::loadModel);
    connect(this, &MainWindow::requestGenerate, engine_, &LlmEngine::generate);
    connect(this, &MainWindow::requestReset, engine_, &LlmEngine::resetConversation);

    connect(engine_, &LlmEngine::loadProgress, this, [this](double f) { loadBar_->setValue(int(f * 100)); });
    connect(engine_, &LlmEngine::modelInfoReady, this, [this](const ModelInfo& info) { modelInfo_ = info; lastLocalTps_ = 0; });
    connect(engine_, &LlmEngine::modelLoaded, this, &MainWindow::onModelLoaded);
    connect(engine_, &LlmEngine::modelFailed, this, &MainWindow::onModelFailed);
    connect(engine_, &LlmEngine::started, this, [this](bool think, int prompt, int reused) {
        startsInThink_ = think;
        statsLabel_->setText(tr("Prompt : %1 tokens (%2 en cache) — génération…").arg(prompt).arg(reused));
    });
    connect(engine_, &LlmEngine::token, this, &MainWindow::onToken);
    connect(engine_, &LlmEngine::finished, this, &MainWindow::onFinished);
    connect(engine_, &LlmEngine::failed, this, &MainWindow::onFailed);
    thread_->start();

    // --- Modèle distant (mêmes signaux que le moteur local)
    remote_ = new RemoteEngine(this);
    connect(this, &MainWindow::requestRemoteGenerate, remote_, &RemoteEngine::generate);
    connect(remote_, &RemoteEngine::started, this, [this](bool think, int, int) {
        startsInThink_ = think;
        statsLabel_->setText(tr("Question envoyée au modèle distant — en attente de la réponse…"));
    });
    connect(remote_, &RemoteEngine::token, this, &MainWindow::onToken);
    connect(remote_, &RemoteEngine::finished, this, &MainWindow::onFinished);
    connect(remote_, &RemoteEngine::failed, this, &MainWindow::onFailed);
    connect(remote_, &RemoteEngine::toolCallsRequested, this, &MainWindow::onToolCalls);
    connect(remote_, &RemoteEngine::toolsUnavailable, this, [this](const QString& why) {
        statusBar()->showMessage(tr("Ce modèle n'accepte pas l'appel d'outils : SeldonH utilise à la place le bloc de calcul dans la réponse."), 9000);
        chat_->addNotice(tr("<i>Le serveur a refusé l'appel d'outils (%1). Les calculs passeront par un bloc de calcul dans la réponse ; "
                            "vous pouvez désactiver l'appel d'outils dans les Préférences.</i>").arg(why.toHtmlEscaped()));
        // Mémorisé pour ce serveur et ce modèle : le mode automatique ne déclarera plus les outils.
        ToolProbeResult r;
        r.support = ToolSupport::Refused;
        r.detail = why;
        r.when = QDateTime::currentDateTimeUtc();
        toolprobe::store(settings_.remoteUrl, settings_.remoteModel, r);
    });
    toolsProbe_ = new ToolsProbe(this);
    connect(toolsProbe_, &ToolsProbe::finished, this, [this](const ToolProbeResult& r) {
        if (!settings_.useRemote || settings_.remoteToolsMode != QLatin1String("auto")) return;
        if (!r.conclusive()) {
            statusBar()->showMessage(tr("Test de l'appel d'outils impossible (%1) : nouvel essai au prochain lancement.").arg(r.detail), 9000);
            return;
        }
        if (generating_) toolsApplyPending_ = true; else applyRemoteTools(false);
        if (r.usesNativeTools())
            statusBar()->showMessage(tr("Appel d'outils natif vérifié pour %1 : le modèle demandera lui-même les calculs.").arg(settings_.remoteModel), 9000);
        else
            chat_->addNotice(tr("<i>Test de l'appel d'outils pour <code>%1</code> : %2</i>")
                                 .arg(settings_.remoteModel.toHtmlEscaped(), toolprobe::describe(r, false).toHtmlEscaped()));
    });
    applyRemoteTools(false);

    showWelcome();
    refreshHistory();
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::saveCurrent);

    // Calculs longs (MCMC d'un événement) : avancement dans la barre d'état, interface redessinée pendant le calcul.
    MethodRunner::setProgressHandler([this](const QString& what, double f) {
        statsLabel_->setText(tr("%1 : %2 %").arg(what).arg(int(std::lround(f * 100.0))));
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        return true;
    });

    QString model = settings_.modelPath;
    if (model.isEmpty() || !QFileInfo::exists(model)) model = AppSettings::findEmbeddedModel();
    if (remoteActive()) activateRemote();
    else if (!model.isEmpty()) loadModel(model);
    else chat_->addNotice(tr("Aucun modèle n'est chargé. Déposez un fichier .gguf dans le dossier des modèles "
                             "(<a href=\"seldon://open-models/0\">ouvrir le dossier</a>, puis relancez), "
                             "ou <a href=\"seldon://choose-model/0\">choisissez un fichier .gguf…</a><br>"
                             "<small>Dossier : %1</small>").arg(AppSettings::modelsDropDir().toHtmlEscaped()));
    updateControls();
}

MainWindow::~MainWindow()
{
    MethodRunner::setProgressHandler({});
    if (remote_) remote_->stop();
    if (engine_) engine_->stop();
    if (thread_) {
        thread_->quit();
        thread_->wait();
    }
}

// ------------------------------------------------------------------ interface

void MainWindow::buildUi()
{
    chat_ = new ChatView;
    chat_->setShowThinking(settings_.showThinking);
    connect(chat_, &QTextBrowser::anchorClicked, this, &MainWindow::onLink);

    input_ = new InputEdit;
    input_->setPlaceholderText(tr("Posez une question (ex. « C14 de 3560 ± 20 BP »), collez un tableau de datations, ou joignez avec 📎 "
                                  "des traces Chain_*.csv, des journaux Log_*.html, un projet .chr ou un tableau .xlsx / .ods / .csv"));
    connect(input_, &InputEdit::submitted, this, &MainWindow::sendMessage);

    sendBtn_ = new QPushButton(tr("Envoyer"));
    sendBtn_->setDefault(true);
    connect(sendBtn_, &QPushButton::clicked, this, &MainWindow::sendMessage);
    stopBtn_ = new QPushButton(tr("Arrêter"));
    connect(stopBtn_, &QPushButton::clicked, this, &MainWindow::stopGeneration);
    // Mode réflexion, sous « Envoyer » : même réglage que la case des Préférences (gardé d'une session à l'autre).
    thinkBtn_ = new QToolButton;
    thinkBtn_->setText(tr("💭 Réfléchir"));
    thinkBtn_->setCheckable(true);
    thinkBtn_->setChecked(settings_.thinking);
    thinkBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    thinkBtn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(thinkBtn_, &QToolButton::toggled, this, [this](bool on) {
        if (settings_.thinking == on) return;
        settings_.thinking = on;
        settings_.save();
        statusBar()->showMessage(on ? tr("Réflexion activée : le modèle raisonne avant de répondre (plus lent).")
                                    : tr("Réflexion désactivée."), 4000);
    });

    attachBtn_ = new QToolButton;
    attachBtn_->setText(tr("📎 Joindre"));
    attachBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    attachBtn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);   // même largeur que « Envoyer »
    attachBtn_->setToolTip(tr("Joindre un ou plusieurs fichiers (.chr, .csv, .xlsx, .html…) ; un nouveau clic en ajoute d'autres"));
    connect(attachBtn_, &QToolButton::clicked, this, &MainWindow::attachFile);

    attachLabel_ = new QLabel;
    attachClear_ = new QToolButton;
    attachClear_->setText(QStringLiteral("✕"));
    connect(attachClear_, &QToolButton::clicked, this, &MainWindow::clearAttachment);
    attachLabel_->hide();
    attachClear_->hide();

    auto* attachRow = new QHBoxLayout;
    attachRow->addWidget(attachLabel_);
    attachRow->addWidget(attachClear_);
    attachRow->addStretch();

    // Colonne de boutons à droite de la saisie : joindre un fichier au-dessus d'« Envoyer » (la saisie prend toute la largeur).
    auto* buttons = new QVBoxLayout;
    buttons->addWidget(attachBtn_);
    buttons->addWidget(sendBtn_);
    buttons->addWidget(stopBtn_);
    buttons->addWidget(thinkBtn_);
    buttons->addStretch();

    auto* inputRow = new QHBoxLayout;
    inputRow->addWidget(input_, 1);
    inputRow->addLayout(buttons);

    // Saisie redimensionnable : poignée entre la conversation et la zone de saisie (tirer vers le haut pour l'agrandir).
    // La hauteur choisie est retenue d'une session à l'autre.
    auto* bottom = new QWidget;
    auto* bottomLay = new QVBoxLayout(bottom);
    bottomLay->setContentsMargins(0, 0, 0, 0);
    bottomLay->addLayout(attachRow);
    bottomLay->addLayout(inputRow, 1);
    input_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    input_->setMinimumHeight(input_->fontMetrics().lineSpacing() * 2 + 16);

    inputSplitter_ = new QSplitter(Qt::Vertical);
    inputSplitter_->addWidget(chat_);
    inputSplitter_->addWidget(bottom);
    inputSplitter_->setStretchFactor(0, 1);
    inputSplitter_->setStretchFactor(1, 0);
    inputSplitter_->setCollapsible(0, false);
    inputSplitter_->setCollapsible(1, false);
    inputSplitter_->setChildrenCollapsible(false);
    inputSplitter_->handle(1)->setToolTip(tr("Tirer pour agrandir ou réduire la zone de saisie"));
    {
        const int saved = QSettings().value("ui/inputHeight", 0).toInt();
        const int h = saved > 0 ? saved : input_->sizeHint().height() + 8;
        inputSplitter_->setSizes({ 600, h });
    }
    connect(inputSplitter_, &QSplitter::splitterMoved, this, [this] {
        const QList<int> sz = inputSplitter_->sizes();
        if (sz.size() == 2) QSettings().setValue("ui/inputHeight", sz[1]);
    });

    // Barre de titre au-dessus de la discussion : nouvelle conversation et titre de la conversation en cours
    // (accessible même quand le volet Historique est replié).
    newConvBtn_ = new QToolButton;
    {
        QSvgRenderer svg(QStringLiteral(":/icons/nouvelle_conversation.svg"));
        QIcon icon;
        for (const int px : { 16, 20, 24, 32, 40, 48, 64 }) {
            QPixmap pm(px, px);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            svg.render(&p);
            icon.addPixmap(pm);
        }
        newConvBtn_->setIcon(icon);
    }
    newConvBtn_->setIconSize(QSize(24, 24));
    newConvBtn_->setAutoRaise(true);
    newConvBtn_->setCursor(Qt::PointingHandCursor);
    newConvBtn_->setAccessibleName(tr("Nouvelle conversation"));
    newConvBtn_->setToolTip(tr("<b>Nouvelle conversation</b> (%1)<br>Commence une conversation vide ; "
                               "la conversation actuelle reste dans l'historique.")
                                .arg(QKeySequence(QKeySequence::New).toString(QKeySequence::NativeText)));
    connect(newConvBtn_, &QToolButton::clicked, this, &MainWindow::newConversation);
    convTitle_ = new QLabel;
    convTitle_->setTextFormat(Qt::PlainText);
    convTitle_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);     // un titre long ne force pas la largeur
    {
        QFont f = convTitle_->font();
        f.setBold(true);
        convTitle_->setFont(f);
    }
    auto* titleBar = new QHBoxLayout;
    titleBar->setContentsMargins(0, 0, 0, 2);
    titleBar->setSpacing(6);
    titleBar->addWidget(newConvBtn_);
    titleBar->addWidget(convTitle_, 1);
    updateConvTitle();

    auto* left = new QWidget;
    auto* leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(6, 6, 6, 6);
    leftLay->addLayout(titleBar);
    leftLay->addWidget(inputSplitter_, 1);

    figures_ = new FigurePanel;
    auto* right = new QWidget;
    auto* rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(6, 6, 6, 6);
    rightLay->addWidget(figures_);

    splitter_ = new QSplitter(Qt::Horizontal);
    splitter_->addWidget(left);
    splitter_->addWidget(right);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({ 760, 520 });

    // Volet « Historique » à gauche, repliable : une fine bande reste toujours visible pour le rouvrir.
    // [historique | bande | (dialogue | graphiques)] : volet ouvert, la bande est à DROITE de l'historique ;
    // volet replié (historique masqué), elle se retrouve au bord gauche de la fenêtre.
    history_ = new HistoryPanel;
    history_->setMinimumWidth(200);
    historyStrip_ = new CollapseStrip(tr("Historique des conversations"));
    historyStrip_->setToolTip(tr("Afficher ou replier l'historique des conversations (Ctrl+Maj+H)"));
    historyStrip_->setFixedWidth(historyStrip_->sizeHint().width());

    outer_ = new QSplitter(Qt::Horizontal);
    outer_->addWidget(history_);
    outer_->addWidget(historyStrip_);
    outer_->addWidget(splitter_);
    outer_->setStretchFactor(0, 0);
    outer_->setStretchFactor(1, 0);
    outer_->setStretchFactor(2, 1);
    for (int i = 0; i < 3; ++i) outer_->setCollapsible(i, false);
    outer_->setSizes({ 270, historyStrip_->width(), 988 });
    // Seule la poignée entre l'historique et la bande redimensionne le volet ; celle d'après est inerte.
    outer_->handle(2)->setEnabled(false);
    outer_->handle(2)->setCursor(Qt::ArrowCursor);
    setCentralWidget(outer_);
    connect(historyStrip_, &QAbstractButton::clicked, this, [this] { setHistoryVisible(historyStrip_->isChecked()); });
    connect(history_, &HistoryPanel::openRequested, this, &MainWindow::openConversation);
    connect(history_, &HistoryPanel::deleteRequested, this, &MainWindow::deleteConversation);
    connect(history_, &HistoryPanel::deleteAllRequested, this, &MainWindow::deleteAllConversations);

    modelLabel_ = new QLabel(tr("Aucun modèle"));
    statsLabel_ = new QLabel;
    loadBar_ = new QProgressBar;
    loadBar_->setRange(0, 100);
    loadBar_->setMaximumWidth(160);
    loadBar_->hide();
    statusBar()->addWidget(modelLabel_);
    statusBar()->addWidget(loadBar_);
    statusBar()->addPermanentWidget(statsLabel_);
    quotaLabel_ = new QLabel;
    quotaLabel_->setTextFormat(Qt::RichText);
    statusBar()->addPermanentWidget(quotaLabel_);
}

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(tr("&Fichier"));
    file->addAction(tr("Nouvelle conversation"), QKeySequence::New, this, &MainWindow::newConversation);
    file->addAction(tr("Ouvrir un modèle (.gguf)…"), QKeySequence::Open, this, &MainWindow::chooseModel);
    file->addAction(tr("Ouvrir le dossier des modèles"), this, &MainWindow::openModelsFolder);
    file->addAction(tr("Joindre des fichiers…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), this, &MainWindow::attachFile);
    file->addAction(tr("Exporter la conversation (HTML ou Word)…"), QKeySequence::Save, this, &MainWindow::saveTranscript);
    file->addSeparator();
    file->addAction(tr("Quitter"), QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu* edit = menuBar()->addMenu(tr("&Édition"));
    QAction* prefs = edit->addAction(tr("Préférences…"), this, &MainWindow::openPreferences);
    prefs->setMenuRole(QAction::PreferencesRole);

    QMenu* sk = menuBar()->addMenu(tr("&Compétences"));
    sk->addAction(tr("Ouvrir le dossier des compétences"), this, &MainWindow::openSkillsFolder);
    sk->addAction(tr("Recharger les compétences"), this, &MainWindow::reloadSkills);

    // Courbes de référence : dossier de l'utilisateur (14C, F14C, AM) et, en option, celles de ChronoModel.
    QMenu* cv = menuBar()->addMenu(tr("Co&urbes"));
    cv->addAction(tr("Courbes disponibles"), this, &MainWindow::showCurves);
    cv->addAction(tr("Ouvrir le dossier des courbes"), this, &MainWindow::openCurvesFolder);
    QAction* cm = cv->addAction(tr("Utiliser aussi les courbes de ChronoModel"));
    cm->setCheckable(true);
    cm->setChecked(settings_.useChronoModelCurves);
    cm->setToolTip(AppSettings::chronoModelCalibDir());
    connect(cm, &QAction::toggled, this, [this](bool on) {
        settings_.useChronoModelCurves = on;
        settings_.save();
        reloadCurves();
    });
    cv->addAction(tr("Recharger les courbes"), this, &MainWindow::reloadCurves);

    QMenu* view = menuBar()->addMenu(tr("&Affichage"));
    QAction* fig = view->addAction(tr("Panneau graphiques"));
    figAction_ = fig;
    fig->setCheckable(true);
    fig->setShortcut(QKeySequence(Qt::Key_F4));
    connect(fig, &QAction::toggled, this, [this](bool on) { splitter_->widget(1)->setVisible(on); });
    // Fermé au démarrage : les figures sont rappelées en vignettes dans le dialogue ; un clic (ou F4) ouvre le panneau.
    fig->setChecked(false);
    splitter_->widget(1)->setVisible(false);
    historyAction_ = view->addAction(tr("Historique des conversations"));
    historyAction_->setCheckable(true);
    historyAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
    connect(historyAction_, &QAction::toggled, this, &MainWindow::setHistoryVisible);
    setHistoryVisible(QSettings().value("ui/historyVisible", true).toBool());

    QMenu* help = menuBar()->addMenu(tr("&Aide"));
    help->addAction(tr("Mode d'emploi"), QKeySequence::HelpContents, this, &MainWindow::showHelp);
    help->addAction(tr("Mode d'emploi dans le navigateur"), this, &MainWindow::openHelpInBrowser);
    help->addSeparator();
    QAction* about = help->addAction(tr("À propos de SeldonH"), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
}

void MainWindow::showWelcome()
{
    auto ex = [](const QString& text) {
        return QStringLiteral("<a href=\"seldon://insert?q=%1\">%2</a>")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(text)), text.toHtmlEscaped());
    };
    // Les exemples sont vérifiés par le test welcomeExamples (tests/tst_core.cpp) : les garder identiques.
    // Assemblage par concaténation : un QString::arg() appliqué APRÈS l'insertion des liens remplacerait les « %3A »,
    // « %3F »… de leurs adresses comme des marqueurs %3 (texte des exemples abîmé).
    const QString sep = QStringLiteral(" · ");
    QString h = tr("<b>Bienvenue dans SeldonH</b><br>"
                   "%1. Cliquez sur un exemple : il est recopié dans la zone de question, vous pouvez le modifier avant d'appuyer sur Entrée.")
                    .arg(settings_.useRemote ? tr("Assistant utilisant un <b>modèle distant</b> (vos questions sont envoyées au serveur configuré ; "
                                                  "les calculs de datation restent locaux)")
                                             : tr("Assistant local (aucune donnée ne quitte votre ordinateur)"));
    h += QStringLiteral("<ul><li>") + tr("<b>Datations simples</b> : ")
         + ex(QStringLiteral("C14 de 3560 +- 20 BP")) + sep + ex(QStringLiteral("archéomagnétisme : inclinaison 65°, alpha95 = 1")) + sep
         + ex(QStringLiteral("date uniforme entre -500 et -400")) + QStringLiteral("</li>");
    h += QStringLiteral("<li>") + tr("<b>Calcul rapide d'un Event</b> (MCMC fait par SeldonH, sans contrainte ni phase) : ")
         + ex(QStringLiteral("Calcule l'événement Four avec C14 2450 ± 30 BP et TL 2600 ± 80 mesurée en 2025")) + QStringLiteral("</li>");
    h += QStringLiteral("<li>") + tr("<b>Fabrique de modèle</b> ChronoModel (<code>.chr</code> : événements, phases, contraintes) : ")
         + ex(QStringLiteral("Crée un modèle ChronoModel : phase A avec US 12 (C14 2450 ± 30 BP), phase B avec US 10 "
                             "(C14 2350 ± 30 BP), US 12 sous US 10")) + QStringLiteral("</li>");
    h += QStringLiteral("<li>") + tr("<b>Contrôle de la convergence</b> d'un calcul ChronoModel :") + QStringLiteral("<ul><li>")
         + tr("à partir des <b>traces</b> (joignez avec 📎 <code>Chain_all_Events.csv</code> ou <code>Chain_all_Phases.csv</code>, "
              "onglet Résultats &gt; Export) : R̂ et ESS de la quantité calculée — ")
         + ex(QStringLiteral("Durée de la phase A : la convergence est-elle satisfaisante ?")) + QStringLiteral("</li><li>")
         + tr("par l'<b>exploration du Log</b> (joignez <code>Log_Posterior_Distrib_Stats.html</code> et les autres journaux, "
              "onglet Résultats &gt; Log) : variables non convergées et remèdes — ")
         + ex(QStringLiteral("Le modèle a-t-il convergé ? Quelles variables posent problème et que faire ?")) + QStringLiteral("</li></ul></li>");
    h += QStringLiteral("<li>") + tr("<b>Post-traitement</b> des traces jointes, tirage par tirage :") + QStringLiteral("<ul><li>")
         + tr("écart entre deux Events : ") + ex(QStringLiteral("Quel est l'écart entre l'Event US 12 et l'Event US 14 ?"))
         + QStringLiteral("</li><li>") + tr("avec le calcul <i>relation</i> : ")
         + tr("ordre ") + ex(QStringLiteral("US 12 est-il antérieur à US 14 ?")) + sep
         + tr("date entre ") + ex(QStringLiteral("Date d'un niveau situé entre US 12 et US 14")) + sep
         + tr("appartenance ") + ex(QStringLiteral("US 5 appartient-il à la phase A ?")) + sep
         + tr("densité d'une borne ") + ex(QStringLiteral("Début de la phase A")) + sep
         + tr("corrélation ") + ex(QStringLiteral("Corrélation entre US 12 et US 14")) + sep
         + tr("avec une datation hors modèle ") + ex(QStringLiteral("Combien de temps entre la fin de la phase A et Ly-1 2450 ± 30 BP ?"))
         + QStringLiteral("</li></ul></li>");
    h += QStringLiteral("<li>") + tr("<b>Courbes de ChronoModel</b> (mode courbe ; joignez avec 📎 <code>Curve_X_Splines.csv</code>, et "
                                     "<code>Chain_all_Events.csv</code> du même calcul pour la convergence, onglet Résultats &gt; Export), "
                                     "chaque courbe tirée évaluée comme dans ChronoModel :") + QStringLiteral("<ul><li>")
         + tr("vitesse entre deux dates ") + ex(QStringLiteral("Vitesse de la courbe entre -500 et 300")) + sep
         + tr("vitesse instantanée ") + ex(QStringLiteral("Vitesse instantanée de la courbe en 300 av. J.-C., par siècle"))
         + QStringLiteral("</li><li>")
         + tr("vitesse entre deux valeurs ") + ex(QStringLiteral("Vitesse de la courbe entre X = -10 et X = 10")) + sep
         + tr("date où la courbe atteint une valeur ") + ex(QStringLiteral("À quelle date la courbe atteint X = 0 ?"))
         + QStringLiteral("</li><li>")
         + tr("probabilité d'une valeur à une date ") + ex(QStringLiteral("Probabilité que X = 10 en 200")) + sep
         + ex(QStringLiteral("Probabilité que la courbe soit comprise entre -10 et 10 en l'an 0"))
         + QStringLiteral("</li></ul></li></ul>");
    h += QStringLiteral("<p>") + tr("Remplacez les noms (US 12, phase A, X…) et les valeurs par ceux de vos fichiers.") + QStringLiteral("</p>");
    h += QStringLiteral("<p>") + tr("📖 <b><a href=\"seldon://help\">Mode d'emploi</a></b> (F1 ou <code>/aide</code>)") + QStringLiteral("<br>")
         + tr("Les compétences de l'assistant sont des fiches Markdown que vous pouvez enrichir (menu Compétences).") + QStringLiteral("</p>");
    chat_->addNotice(h);
}

void MainWindow::showHelp()
{
    if (!help_) {
        help_ = new seldon::HelpView;
        connect(help_, &seldon::HelpView::insertRequested, this, &MainWindow::insertExample);
    }
    showFiguresPanel();
    figures_->showPage(help_, tr("Aide"));
}

void MainWindow::openHelpInBrowser()
{
    const QString path = seldon::HelpView::exportForBrowser();
    if (path.isEmpty() || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        QMessageBox::warning(this, tr("Mode d'emploi"), tr("Impossible d'ouvrir le mode d'emploi dans le navigateur."));
}

void MainWindow::insertExample(const QString& text)
{
    input_->setPlainText(text);
    input_->moveCursor(QTextCursor::End);
    input_->setFocus();
}

// ---------------------------------------------------------------- modèle / état

void MainWindow::loadModel(const QString& path)
{
    modelReady_ = false;
    localCtx_ = 0;
    resetQuota();
    loading_ = true;
    modelLabel_->setText(tr("Chargement de %1…").arg(QFileInfo(path).fileName()));
    loadBar_->setValue(0);
    loadBar_->show();
    updateControls();
    emit requestLoad(path, settings_.nCtx, settings_.nThreads, settings_.nGpuLayers);
}

void MainWindow::activateRemote()
{
    loading_ = false;
    loadBar_->hide();
    RemoteConfig cfg;
    cfg.baseUrl = settings_.remoteUrl;
    cfg.apiKey  = settings_.remoteKey;
    cfg.model   = settings_.remoteModel;
    remote_->setConfig(cfg);
    applyRemoteTools(true);
    remoteCtx_ = 0;
    resetQuota();
    if (!cfg.isValid()) {
        modelReady_ = false;
        modelLabel_->setText(tr("Modèle distant non configuré"));
        chat_->addNotice(tr("<b>Modèle distant incomplet.</b> Renseignez la Base URL et le nom du modèle dans "
                            "<i>Édition &gt; Préférences</i>."));
    } else {
        modelReady_ = true;
        modelDesc_ = cfg.model;
        modelLabel_->setText(tr("☁ Modèle distant : %1 — %2").arg(cfg.model, remote::hostOf(cfg.baseUrl)));
        fetchRemoteContext();
        chat_->addNotice(tr("☁ <b>Modèle distant actif</b> : <code>%1</code> sur <code>%2</code>. Vos questions, les compétences "
                            "ajoutées et les fichiers joints sont envoyés à ce serveur ; les calculs de datation restent faits "
                            "sur cet ordinateur.").arg(cfg.model.toHtmlEscaped(), remote::hostOf(cfg.baseUrl).toHtmlEscaped()));
    }
    updateControls();
}

void MainWindow::applyRemoteTools(bool probeIfNeeded)
{
    if (!remote_) return;
    ToolProbeResult c;
    const bool has = toolprobe::cached(settings_.remoteUrl, settings_.remoteModel, c);
    remote_->setTools(toolprobe::wantTools(settings_.remoteToolsMode, has ? &c : nullptr) ? tools::schemaForApi() : QJsonArray());
    if (!probeIfNeeded || !settings_.useRemote || settings_.remoteToolsMode != QLatin1String("auto")) return;
    if (has && toolprobe::isFresh(c)) return;
    RemoteConfig cfg;
    cfg.baseUrl = settings_.remoteUrl;
    cfg.apiKey  = settings_.remoteKey;
    cfg.model   = settings_.remoteModel;
    if (cfg.isValid() && toolsProbe_ && !toolsProbe_->running()) toolsProbe_->start(cfg);
}

void MainWindow::onModelLoaded(const QString& description, int ctx)
{
    loading_ = false;
    loadBar_->hide();
    localCtx_ = ctx;
    // Chargement local terminé alors que le modèle distant est actif (changement de moteur pendant le chargement) :
    // l'indication reste celle du modèle distant, qui répond aux questions.
    if (remoteActive()) { updateControls(); return; }
    modelReady_ = true;
    modelDesc_ = description;
    loadBar_->hide();
    modelLabel_->setText(tr("Modèle : %1 — contexte %2 — %3")
                             .arg(description).arg(ctx).arg(settings_.nGpuLayers != 0 ? tr("GPU (Metal)") : tr("CPU")));
    updateControls();
}

void MainWindow::onModelFailed(const QString& error)
{
    loading_ = false;
    loadBar_->hide();
    if (remoteActive()) { updateControls(); return; }   // le modèle distant reste celui qui répond
    modelReady_ = false;
    modelLabel_->setText(tr("Aucun modèle"));
    chat_->addNotice(tr("<b>Échec du chargement du modèle.</b><br>%1<br>"
                        "<a href=\"seldon://choose-model/0\">Choisir un autre fichier .gguf…</a>").arg(error.toHtmlEscaped()));
    updateControls();
}

void MainWindow::updateControls()
{
    sendBtn_->setEnabled(!generating_);          // sans modèle, seule la commande /aide est utile (voir sendMessage)
    stopBtn_->setVisible(generating_);
    sendBtn_->setVisible(!generating_);
    if (history_) history_->setBusy(generating_);
    if (newConvBtn_) newConvBtn_->setEnabled(!generating_);
    if (chat_) chat_->setEditEnabled(!generating_);
    updateThinkButton();
}

bool MainWindow::thinkingSupported() const
{
    return modelReady_ && !remoteActive() && modelInfo_.supportsThinking;
}

void MainWindow::updateThinkButton()
{
    if (!thinkBtn_) return;
    const bool ok = thinkingSupported();
    thinkBtn_->setEnabled(ok && !generating_);
    { const QSignalBlocker block(thinkBtn_); thinkBtn_->setChecked(settings_.thinking); }
    thinkBtn_->setToolTip(ok ? tr("Le modèle raisonne avant de répondre : plus lent, utile pour les demandes complexes (modèle "
                                  "ChronoModel, interprétation des résultats). Pour voir son raisonnement : Préférences > "
                                  "« Afficher le raisonnement du modèle ».")
                             : tr("Sans effet sur ce modèle"));
}

void MainWindow::updateConvTitle()
{
    if (!convTitle_) return;
    QString title;
    for (const Turn& t : std::as_const(turns_))
        if (!t.synthetic) { title = Conversation::makeTitle(t.question); break; }
    const bool empty = title.isEmpty();
    if (empty) title = tr("Nouvelle conversation");
    convTitle_->setText(title);
    convTitle_->setToolTip(empty ? QString() : title);
    convTitle_->setEnabled(!empty);                   // grisé tant qu'aucune question n'a été posée
}

void MainWindow::chooseModel()
{
    const QString start = settings_.modelPath.isEmpty() ? AppSettings::defaultModelsDir()
                                                        : QFileInfo(settings_.modelPath).absolutePath();
    const QString f = QFileDialog::getOpenFileName(this, tr("Choisir un modèle GGUF"), start,
                                                   tr("Modèles GGUF (*.gguf)"));
    if (f.isEmpty()) return;
    settings_.modelPath = f;
    settings_.save();
    loadModel(f);
}

void MainWindow::openPreferences()
{
    SettingsDialog dlg(settings_, this);
    dlg.setLocalModel(modelInfo_, lastLocalTps_);     // modèle local chargé (même si le moteur distant est actif)
    if (dlg.exec() != QDialog::Accepted) return;
    AppSettings n = dlg.settings();

    const bool remoteChanged = n.useRemote != settings_.useRemote
                               || (n.useRemote && (n.remoteUrl != settings_.remoteUrl || n.remoteKey != settings_.remoteKey
                                                   || n.remoteModel != settings_.remoteModel));
    if (remoteChanged && generating_) {
        // On ne change pas de moteur au milieu d'une réponse.
        n.useRemote = settings_.useRemote; n.remoteUrl = settings_.remoteUrl;
        n.remoteKey = settings_.remoteKey; n.remoteModel = settings_.remoteModel;
        statusBar()->showMessage(tr("Moteur inchangé : une réponse est en cours. Réessayez après la fin."), 6000);
    } else if (n.useRemote && (!settings_.useRemote || remote::hostOf(n.remoteUrl) != remote::hostOf(settings_.remoteUrl))) {
        // Consentement explicite : les données quittent l'ordinateur.
        const QString host = remote::hostOf(n.remoteUrl).isEmpty() ? n.remoteUrl : remote::hostOf(n.remoteUrl);
        const auto ans = QMessageBox::question(
            this, tr("Modèle distant"),
            tr("Avec un modèle distant, vos questions, les compétences ajoutées et les fichiers joints seront envoyés à « %1 ».\n\n"
               "Les calculs de datation restent faits sur cet ordinateur. Continuer ?").arg(host));
        if (ans != QMessageBox::Yes) {
            n.useRemote = settings_.useRemote; n.remoteUrl = settings_.remoteUrl;
            n.remoteKey = settings_.remoteKey; n.remoteModel = settings_.remoteModel;
        }
    }

    const bool reload = n.modelPath != settings_.modelPath || n.nCtx != settings_.nCtx
                        || n.nThreads != settings_.nThreads || n.nGpuLayers != settings_.nGpuLayers;
    const bool skillsMoved = n.skillsDir != settings_.skillsDir;
    const bool wasRemote = settings_.useRemote;
    const bool remoteNow = n.useRemote;
    const bool remoteSettingsChanged = remoteNow && (!wasRemote || n.remoteUrl != settings_.remoteUrl
                                                     || n.remoteKey != settings_.remoteKey || n.remoteModel != settings_.remoteModel);
    settings_ = n;
    settings_.save();
    dateformat::setDecimals(settings_.decimals);
    chat_->setShowThinking(settings_.showThinking);
    updateThinkButton();
    if (remote_) applyRemoteTools(settings_.useRemote && !remoteSettingsChanged);
    if (skillsMoved) reloadSkills();

    if (remoteNow) {
        if (remoteSettingsChanged) activateRemote();
    } else if (wasRemote) {
        // retour au modèle local
        modelReady_ = false;
        modelLabel_->setText(tr("Aucun modèle"));
        QString model = settings_.modelPath;
        if (model.isEmpty() || !QFileInfo::exists(model)) model = AppSettings::findEmbeddedModel();
        if (!model.isEmpty() && !generating_) loadModel(model);
        else updateControls();
    } else if (reload && !settings_.modelPath.isEmpty() && !generating_) {
        loadModel(settings_.modelPath);
    }
}

// ------------------------------------------------------------------ conversation

// Dates d'un .chr joint : le modèle de langage oublie souvent leurs noms en écrivant le calcul (voir fillDateNames).
void MainWindow::rememberChrDates(const QJsonObject& chr)
{
    for (const QJsonValue& e : chr.value("events").toArray())
        for (const QJsonValue& d : e.toObject().value("dates").toArray())
            if (d.isObject()) chrDates_ << d.toObject();
}

// Message d'un tour passé tel que renvoyé dans l'historique : sans ses fiches de connaissances (celles de la question
// en cours sont jointes à son propre message). Les garder ferait grossir chaque tour de 10 à 20 000 caractères et
// dépasser le contexte d'un modèle distant (32 k tokens pour ministral-8b) après quelques questions.
QString MainWindow::historyUser(const Turn& t)
{
    // Résumé du tableau courant : envoyé avec chaque question, inutile de le garder dans les tours passés.
    QString mu = t.modelUser;
    static const QString tableHead = QStringLiteral("[Tableau courant « ");
    static const QString tableEnd = QStringLiteral("N'écris jamais les valeurs du tableau toi-même.]\n");
    for (int a = mu.indexOf(tableHead); a >= 0; a = mu.indexOf(tableHead, a)) {
        const int b = mu.indexOf(tableEnd, a);
        if (b < 0) break;
        mu.remove(a, b + tableEnd.size() - a);
    }
    static const QString head = QStringLiteral("[Connaissances de référence]\n");
    const int a = mu.indexOf(head);
    if (a < 0) return mu;
    int b = -1;
    for (const QString& next : { QStringLiteral("\n[Fichier joint : "), QStringLiteral("\n[Résultats ChronoModel"),
                                 QStringLiteral("\n[SeldonH a dessiné"), QStringLiteral("\n[Question]\n") }) {
        const int k = mu.indexOf(next, a + head.size());
        if (k >= 0 && (b < 0 || k < b)) b = k + 1;
    }
    return b < 0 ? mu.left(a) : mu.left(a) + mu.mid(b);
}

// Tour passé en mode calculatrice (résultat affiché par SeldonH, sans réponse du modèle), mis en contexte du message suivant.
QString MainWindow::calcOnlyContext(const Turn& t)
{
    return QStringLiteral("[Échange précédent : question de l'utilisateur et résultat affiché par SeldonH, sans réponse du modèle]\n")
           + historyUser(t) + QStringLiteral("\n[Fin de l'échange précédent]\n\n");
}

QVector<ChatMessage> MainWindow::buildChat(QJsonArray* refs) const
{
    QVector<ChatMessage> v;
    const std::string userPrompt = settings_.userPrompt.toStdString();
    const std::string system = skills_.alwaysText().toStdString() + dateformat::systemHint(dateformat::fromId(settings_.dateFormat))
                               + ((remoteActive() && remote_ && remote_->toolsActive()) ? toolsSystemHint() : std::string()) + userPromptSystemBlock(userPrompt);
    v.append({ "system", system });
    if (refs) refs->append(QJsonObject{ { "role", "system" }, { "content", QString::fromStdString(system) } });
    // Mode calculatrice : un tour sans réponse du modèle n'est pas mis en paire question / réponse (une réponse de
    // remplacement serait imitée par un petit modèle, qui la recopierait à la question suivante) ; sa question et son
    // résultat sont placés en contexte au début du message suivant.
    QString context;
    QJsonArray contextTurns;
    for (int i = 0; i < turns_.size(); ++i) {
        const Turn& t = turns_[i];
        const bool last = (i == turns_.size() - 1);
        const QString said = (t.answer.trimmed() + t.toolBlocks).trimmed();     // réponse + appels d'outils réécrits en blocs
        if (!last && said.isEmpty() && t.calcOnly) {
            context += calcOnlyContext(t);
            contextTurns.append(i);
            continue;
        }
        if (!last && said.isEmpty()) continue;       // tour interrompu/en erreur : ignoré
        const std::string suffix = last ? userPromptReminder(userPrompt) : std::string();
        v.append({ "user", (context + (last ? t.modelUser : historyUser(t))).toStdString() + suffix });
        if (refs) {
            QJsonObject u{ { "role", "user" }, { "turn", i } };
            if (!suffix.empty()) u["suffix"] = QString::fromStdString(suffix);
            if (!contextTurns.isEmpty()) u["context_turns"] = contextTurns;
            refs->append(u);
        }
        context.clear();
        contextTurns = QJsonArray();
        if (!last) {
            v.append({ "assistant", said.toStdString() });
            if (refs) refs->append(QJsonObject{ { "role", "assistant" }, { "turn", i } });
        }
    }
    return v;
}

QJsonObject MainWindow::promptSnapshot(const QJsonArray& messages, const SamplingParams& sp) const
{
    // Paramètres en float : arrondis à 6 décimales pour l'export (0.2f s'écrirait 0.20000000298023224).
    auto f6 = [](float v) { return std::round(double(v) * 1e6) / 1e6; };
    QJsonObject engine, sampling{ { "temperature", f6(sp.temperature) }, { "max_tokens", sp.maxTokens }, { "thinking", sp.enableThinking } };
    if (remoteActive()) {
        engine = QJsonObject{ { "type", "distant" }, { "url", settings_.remoteUrl }, { "model", settings_.remoteModel },
                              { "tools", remote_ && remote_->toolsActive() }, { "tools_mode", settings_.remoteToolsMode } };
    } else {
        engine = QJsonObject{ { "type", "local" }, { "model", QFileInfo(settings_.modelPath).fileName() }, { "model_path", settings_.modelPath },
                              { "n_ctx", settings_.nCtx }, { "threads", settings_.nThreads }, { "gpu_layers", settings_.nGpuLayers } };
        sampling["top_k"] = sp.topK;
        sampling["top_p"] = f6(sp.topP);
        sampling["min_p"] = f6(sp.minP);
        sampling["repeat_penalty"] = f6(sp.repeatPenalty);
        sampling["presence_penalty"] = f6(sp.presencePenalty);
        sampling["penalty_last_n"] = sp.penaltyLastN;
        sampling["seed"] = sp.seed == 0xFFFFFFFFu ? QJsonValue(QStringLiteral("aléatoire")) : QJsonValue(double(sp.seed));
    }
    return QJsonObject{ { "date", QDateTime::currentDateTime().toString(Qt::ISODate) },
                        { "seldonh", QCoreApplication::applicationVersion() },
                        { "engine", engine }, { "sampling", sampling }, { "messages", messages } };
}

QJsonArray MainWindow::expandPromptMessages(const QJsonArray& messages) const
{
    QJsonArray out;
    for (const QJsonValue& v : messages) {
        const QJsonObject m = v.toObject();
        QJsonObject e{ { "role", m.value("role") } };
        if (m.contains("content")) {
            e["content"] = m.value("content");
        } else {
            const int k = m.value("turn").toInt(-1);
            if (k < 0 || k >= turns_.size()) continue;
            const Turn& t = turns_[k];
            QString context;                                 // tours « calcul seul » placés en tête (voir buildChat)
            for (const QJsonValue& c : m.value("context_turns").toArray())
                if (c.toInt(-1) >= 0 && c.toInt() < turns_.size()) context += calcOnlyContext(turns_[c.toInt()]);
            e["content"] = m.value("role").toString() == QLatin1String("user")
                               ? context + (k == turns_.size() - 1 ? t.modelUser : historyUser(t)) + m.value("suffix").toString()
                               : (t.answer.trimmed() + t.toolBlocks).trimmed();
        }
        out.append(e);
    }
    return out;
}

void MainWindow::relaunchTurn(int turn)
{
    if (generating_ || turn < 0 || turn >= turns_.size()) return;
    if (!modelReady_) {
        statusBar()->showMessage(tr("Aucun modèle prêt : impossible de relancer la question."), 5000);
        return;
    }
    int q = turn;
    while (q > 0 && turns_[q].synthetic) --q;           // tour de suite d'un calcul : on relance la question d'origine
    if (q == turns_.size() - 1) {                        // dernière question : nouvelle réponse à la place de l'ancienne
        Turn& t = turns_.last();
        t.answer.clear();
        t.toolBlocks.clear();
        t.thinking.clear();
        t.truncation = 0;
        chat_->setActions(t.view, {});
        chat_->setAssistant(t.view, QString(), QString(), false, false);
        statusBar()->showMessage(tr("Question relancée (même prompt, nouvelle réponse)."), 5000);
    } else {                                             // question plus ancienne : reposée à la fin de la conversation
        Turn t;
        const Turn& src = turns_[q];
        t.question = src.question;
        t.modelUser = src.modelUser;                     // même texte envoyé (connaissances, résultats, fichier joint)
        t.skillIds = src.skillIds;
        t.followUp = src.followUp;
        t.drewModel = src.drewModel;
        logUser(src.question + tr("\n\n↻ Question relancée"));
        t.view = newAssistantBubble();
        turns_.append(t);
    }
    saveCurrent();
    startGeneration();
}

void MainWindow::exportPrompt(int turn)
{
    if (turn < 0 || turn >= turns_.size() || turns_[turn].prompt.isEmpty()) return;
    const Turn& t = turns_[turn];
    QJsonObject full = t.prompt;
    full["messages"] = expandPromptMessages(t.prompt.value("messages").toArray());
    QJsonArray suites;
    for (const QJsonValue& v : t.prompt.value("suites").toArray()) {
        QJsonObject s = v.toObject();
        s["messages"] = expandPromptMessages(s.value("messages").toArray());
        suites.append(s);
    }
    if (!suites.isEmpty()) full["suites"] = suites;
    full["question"] = t.question;
    full["skills"] = QJsonArray::fromStringList(t.skillIds);
    full["answer"] = t.answer;                           // réponse obtenue avec ce prompt
    if (!t.toolBlocks.isEmpty()) full["tool_calls"] = t.toolBlocks;

    const QString jsonFilter = tr("Prompt complet, JSON (*.json)"), mdFilter = tr("Prompt lisible, Markdown (*.md)");
    QString selected = jsonFilter;
    const QString base = QStringLiteral("prompt_%1_%2").arg(QDateTime::fromString(t.prompt.value("date").toString(), Qt::ISODate)
                                                                .toString(QStringLiteral("yyyyMMdd_HHmmss")))
                                                       .arg(turn + 1);
    QString path = QFileDialog::getSaveFileName(this, tr("Exporter le prompt"), QDir::homePath() + "/" + base + ".json",
                                                jsonFilter + ";;" + mdFilter, &selected);
    if (path.isEmpty()) return;
    const QString suf = QFileInfo(path).suffix().toLower();
    if (suf != "json" && suf != "md") path += selected == mdFilter ? ".md" : ".json";

    QByteArray data;
    if (QFileInfo(path).suffix().compare("md", Qt::CaseInsensitive) == 0) {
        const QJsonObject eng = full.value("engine").toObject(), smp = full.value("sampling").toObject();
        QString md = tr("# Prompt envoyé au modèle — SeldonH %1\n\n").arg(full.value("seldonh").toString());
        md += tr("- **Date** : %1\n").arg(full.value("date").toString());
        md += eng.value("type").toString() == QLatin1String("distant")
                  ? tr("- **Moteur** : distant, %1 (%2), appel d'outils %3\n").arg(eng.value("model").toString(), eng.value("url").toString(),
                                                                                    eng.value("tools").toBool() ? tr("activé") : tr("désactivé"))
                  : tr("- **Moteur** : local, %1 (contexte %2 tokens)\n").arg(eng.value("model").toString()).arg(eng.value("n_ctx").toInt());
        QStringList params;
        for (auto it = smp.begin(); it != smp.end(); ++it)
            params << QStringLiteral("%1 = %2").arg(it.key(), it.value().isString() ? it.value().toString() : QString::fromUtf8(QJsonDocument(QJsonArray{ it.value() }).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
        md += tr("- **Paramètres** : %1\n").arg(params.join(", "));
        md += tr("- **Compétences injectées** : %1\n").arg(t.skillIds.isEmpty() ? tr("aucune") : t.skillIds.join(", "));
        if (full.contains("dropped_messages"))
            md += tr("- **Messages oubliés par le moteur** (contexte plein) : %1\n").arg(full.value("dropped_messages").toInt());
        auto messages = [&](const QJsonArray& msgs) {
            QString s;
            for (const QJsonValue& v : msgs) {
                const QJsonObject m = v.toObject();
                s += QStringLiteral("\n## %1\n\n````text\n%2\n````\n").arg(m.value("role").toString(), m.value("content").toString());
            }
            return s;
        };
        md += tr("\n# Messages\n") + messages(full.value("messages").toArray());
        for (const QJsonValue& v : std::as_const(suites)) md += tr("\n# Suite de la réponse (réponse coupée)\n") + messages(v.toObject().value("messages").toArray());
        md += tr("\n# Réponse obtenue\n\n````markdown\n%1\n````\n").arg(t.answer);
        data = md.toUtf8();
    } else {
        data = QJsonDocument(full).toJson(QJsonDocument::Indented);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        QMessageBox::warning(this, tr("Exporter le prompt"), tr("Impossible d'écrire %1.").arg(path));
        return;
    }
    statusBar()->showMessage(tr("Prompt exporté : %1").arg(path), 6000);
}

void MainWindow::sendMessage()
{
    // Commande /aide : ouvre le mode d'emploi, sans modèle chargé et sans l'envoyer au modèle.
    static const QRegularExpression helpCmd(QStringLiteral(R"(^\s*/?(aide|help|mode d'emploi)\s*\??\s*$)"),
                                            QRegularExpression::CaseInsensitiveOption);
    if (helpCmd.match(input_->toPlainText()).hasMatch()) {
        input_->clear();
        showHelp();
        return;
    }
    // « relance », « recommence », « réessaie » tapé seul (prompt réel du 05/11/2026 : le modèle a recopié sa réponse précédente) :
    // la dernière question de l'utilisateur est reposée telle quelle, analysée de nouveau par SeldonH.
    if (!generating_) {
        static const QRegularExpression retryCmd(QStringLiteral(R"(^\s*(relance[rz]?|recommence[rz]?|r[ée]essa(ie|ye)[rz]?|refai[st]( le calcul)?|retry|again|try again)\s*[.!]?\s*$)"),
                                                 QRegularExpression::CaseInsensitiveOption);
        if (retryCmd.match(input_->toPlainText()).hasMatch()) {
            for (int k = int(turns_.size()) - 1; k >= 0; --k) {
                if (turns_[k].synthetic || turns_[k].question.trimmed().isEmpty() || retryCmd.match(turns_[k].question).hasMatch()) continue;
                QString prev = turns_[k].question;
                const qsizetype clip = prev.indexOf(QStringLiteral("\n\n📎"));
                if (clip >= 0) prev.truncate(clip);
                input_->setPlainText(prev.trimmed());
                break;
            }
        }
    }

    // Calcul seul (« 2+3 », « ADtoBP(-550) », « a = 3560 ») : calculatrice locale, exacte et instantanée ; rien n'est envoyé
    // au modèle de langage, qui n'a pas besoin d'être chargé.
    if (!generating_) {
        const QString q0 = input_->toPlainText().trimmed();
        if (calc_.isCalculation(q0)) {
            logUser(q0);
            logNotice(calcHtml(calc_.evaluate(q0)));
            input_->clear();
            return;
        }
    }

    // « Dessine le modèle » : scènes du .chr joint, ou du dernier modèle joint ou créé dans la conversation. Fait par
    // l'application (même sans modèle de langage chargé) ; le modèle de langage est seulement prévenu.
    bool drawRequested = false;
    if (!generating_) {
        const QString q0 = input_->toPlainText().trimmed();
        const QString k0 = lexicon_.translate(q0);
        if (ChrSceneWidget::asksForDrawing(k0) && !ChrBuilder::asksForJson(q0)) {
            if (!attachChr_.isEmpty()) { lastChr_ = attachChr_; lastChrTitle_ = attachChrName_; rememberChrDates(attachChr_); }
            if (!lastChr_.isEmpty()) {
                drawRequested = true;
                // Seulement le dessin (ou pas de modèle de langage) : fait par l'application, sans le modèle de langage.
                if (!modelReady_ || ChrSceneWidget::isOnlyDrawing(k0)) {
                    logUser(attachName_.isEmpty() ? q0 : q0 + "\n\n📎 " + attachName_);
                    showChrScenes(lastChr_, lastChrTitle_, true);
                    input_->clear();
                    clearAttachment();
                    saveCurrent();
                    return;
                }
            }
        }
    }

    if (generating_) return;
    const QString qShown = input_->toPlainText().trimmed();
    // Tableau collé dans la question : lu par SeldonH (tableau courant) ; le reste du texte est la consigne. Un tableau non
    // compris n'est jamais lu comme des datations isolées (garde-fou de MethodRunner::requestsFromQuestion).
    QString q = qShown;
    QString tableNotice;
    bool tableOnly = false;
    {
        int ta = -1, tn = 0;
        if (!qShown.isEmpty() && seldon::datatable::looksLikeTable(qShown, &ta, &tn) && !seldon::datatable::isChronoModelImport(qShown)) {
            QString err;
            seldon::datatable::Table pasted = seldon::datatable::parseText(qShown.mid(ta, tn), QString(), &err);
            if (pasted.isValid() && pasted.datable() > 0) {
                seldon::datatable::setCurrent(pasted);
                tableNotice = seldon::datatable::summaryHtml(pasted);
                q = (qShown.left(ta) + QLatin1Char('\n') + qShown.mid(ta + tn)).trimmed();
                while (q.endsWith(QLatin1Char(':'))) { q.chop(1); q = q.trimmed(); }
                tableOnly = q.isEmpty();
            } else {
                tableNotice = tr("<p><i>⚠ Le tableau collé n'a pas été compris par SeldonH (%1) : aucun calcul n'est lancé à partir de lui. "
                                 "Vérifiez la ligne d'en-tête (Nom, Méthode, Âge, Erreur, Année de mesure…).</i></p>").arg(err.toHtmlEscaped());
            }
        }
    }
    // Sans modèle de langage, un simple calcul de datation reste possible (mode calculatrice) : fiche de SeldonH seule.
    // Un tableau joint ou collé est aussi lu sans modèle (fiche « tableau lu »).
    const bool tableHere = attachTable_ || (!tableNotice.isEmpty() && seldon::datatable::hasCurrent());
    const bool calcWithoutModel = !modelReady_ && attachOnlyData()
                                  && (tableOnly || tableHere || !MethodRunner::requestsFromQuestion(q).isEmpty()
                                      || MethodRunner::asksCurveList(lexicon_.translate(q)))
                                  && !wantsCalcComment(q);
    if (!modelReady_ && !calcWithoutModel) {
        statusBar()->showMessage(loading_ ? tr("Chargement du modèle en cours…")
                                 : remoteActive() ? tr("Modèle distant non configuré : Édition > Préférences. La commande /aide fonctionne sans modèle.")
                                                  : tr("Aucun modèle chargé : Fichier > Ouvrir un modèle (.gguf). La commande /aide fonctionne sans modèle."), 6000);
        return;
    }
    if (qShown.isEmpty()) return;
    // Repérage des mots-clés (fiches, détecteurs) : question en espagnol, italien ou allemand ramenée au français.
    const QString qk = lexicon_.translate(q);

    if (convId_.isEmpty()) {                      // première question : la conversation reçoit son identifiant
        convId_ = Conversation::newId();
        convCreated_ = QDateTime::currentDateTime();
    }

    // Demande de création de modèle : la fiche qui décrit le format du bloc chronomodel est toujours fournie.
    // Fiches « Date — … » des méthodes citées : seuls leurs noms de champs sont montrés au modèle (pas de vocabulaires
    // concurrents). Création de modèle : fiche commune + ces fiches, et rien d'autre (prompt court, moins d'erreurs).
    static const QHash<QString, QString> dateSkill = {
        { "c14", "14_date_c14" }, { "f14c", "15_date_f14c" }, { "tl", "16_date_tl" }, { "am", "17_date_am" },
        { "gauss", "08_dates_autres_methodes" }, { "density", "18_date_densite" } };
    const bool modelRequest = MethodRunner::isModelRequest(qk);
    QStringList cited = MethodRunner::datingMethodsIn(qk);
    if (modelRequest && cited.isEmpty()) cited = QStringList{ "c14", "tl", "am", "gauss" };     // méthodes non nommées : les courantes
    QStringList pinned;
    if (modelRequest) pinned << QStringLiteral("03_chronomodel_creer_modele");
    if (MethodRunner::isAgeDepthRequest(qk)) pinned << QStringLiteral("13_chronomodel_courbe_age_profondeur");
    // « Laquelle est la plus ancienne ? », « combien de temps entre A et B ? », « C est-elle entre A et B ? » : la fiche
    // des relations entre dates (probabilité d'antériorité, durée, mélange datations / traces).
    if (!modelRequest && (MethodRunner::asksOrder(qk)
                          || (MethodRunner::asksRelation(qk) && (!cited.isEmpty() || !seldon::traces::loaded().isEmpty()))))
        pinned << QStringLiteral("21_ordre_des_dates");
    // Courbe de ChronoModel jointe (trace des splines, carte de densité) : la fiche du calcul « curve ».
    if (seldon::curves::anyLoaded() && !modelRequest) pinned << QStringLiteral("23_courbes_chronomodel");
    // Tableau de datations joint ou collé : la fiche des actions sur le tableau (sélection, datation, modèle).
    if (seldon::datatable::hasCurrent()) pinned << QStringLiteral("22_tableaux");
    // Traces MCMC jointes dans la conversation : la fiche qui décrit le calcul « traces ».
    if (!seldon::traces::loaded().isEmpty() && !modelRequest) pinned << QStringLiteral("20_combinaison_traces_inverse");
    for (const QString& m : std::as_const(cited)) pinned << dateSkill.value(m);
    // Résultats de ChronoModel : ceux joints à la question, sinon les derniers de la conversation (« fais le tableau des
    // phases » après une première question). Question sur les phases, successions ou Events : fiche dédiée ; sinon,
    // résultats joints : fiche de diagnostic de l'utilisateur si elle existe, sinon la fiche intégrée des logs.
    if (attachChronoResults_) { lastResults_ = attachResults_; lastResultsName_ = attachResultsName_; }
    turnHasResults_ = attachChronoResults_;          // journaux joints à cette question : suites sur la convergence
    const int resultsFocus = lastResultsName_.isEmpty() ? 0 : chronoresults::focusOf(qk);
    if (resultsFocus) {
        pinned << QStringLiteral("19_chronomodel_phases_events");
    } else if (attachChronoResults_) {
        bool user = false;
        for (const Skill& s : skills_.skills()) user = user || s.id == QLatin1String("chronomodel_resultats_convergence");
        pinned << (user ? QStringLiteral("chronomodel_resultats_convergence") : QStringLiteral("11_chronomodel_logs"));
    }
    // Les fiches de création de modèle (« ÉCRIS un bloc chronomodel ») ne sont jointes qu'à une demande de modèle, ou à la
    // suite d'un modèle déjà produit (« ajoute une phase ») : leurs mots-clés (event, wiggle, .chr) les feraient venir dans
    // un simple calcul d'Event, où elles empêchent l'appel de l'outil. Les fiches des tours passés ne sont plus dans
    // l'historique (voir historyUser) : rien d'autre n'est exclu.
    bool modelFollowUp = false;
    for (const Turn& p : std::as_const(turns_)) { QJsonObject spec; modelFollowUp = modelFollowUp || ChrBuilder::extractSpec(p.answer, spec); }
    QStringList exclude;
    // « Durée », « début », « fin » appellent aussi les fiches de lecture des résultats de ChronoModel : sans résultats
    // joints ni question sur ChronoModel, elles n'ont pas lieu d'être (elles chassent les fiches utiles d'un petit modèle).
    static const QRegularExpression aboutChronoModel(QStringLiteral(
        R"(\b(?:chronomodel|phases?|events?|evenements?|mcmc|convergence|chaines?|r\^|ess|posteriori|posterior|log|logs|modeles?|model)\b)"));
    if (lastResultsName_.isEmpty() && !attachChronoResults_ && seldon::traces::loaded().isEmpty()
        && !aboutChronoModel.match(seldon::text::fold(qk)).hasMatch())
        exclude << QStringLiteral("19_chronomodel_phases_events") << QStringLiteral("04_chronomodel_interpreter")
                << QStringLiteral("chronomodel_resultats_convergence");
    if (!modelRequest && !modelFollowUp)
        exclude << QStringLiteral("03_chronomodel_creer_modele") << QStringLiteral("13_chronomodel_courbe_age_profondeur");
    // Question sur l'utilisation de SeldonH, sans calcul (« comment faire une datation ? ») : seulement la fiche d'aide ; la
    // réponse donne des phrases à taper, pas de code (les fiches de calcul pousseraient le modèle à écrire un bloc).
    const bool helpQuestion = !modelRequest && !tableOnly && attachName_.isEmpty() && !resultsFocus && MethodRunner::asksHowTo(qk)
                              && MethodRunner::requestsFromQuestion(q).isEmpty();
    if (helpQuestion) pinned = QStringList{ QStringLiteral("10_aide_utilisation") };
    // Création de modèle : budget 0 → seulement les fiches imposées ci-dessus.
    const QList<SkillMatch> matches = skills_.select(qk, modelRequest ? 0 : settings_.skillChars, exclude, pinned);
    const QString knowledge = SkillManager::format(matches);

    if (!turns_.isEmpty() && turns_.last().truncation) {     // nouvelle question : plus de « Continuer » sur la précédente
        turns_.last().truncation = 0;
        setTurnActions(turns_.size() - 1);
    }

    if (!turns_.isEmpty() && turns_.last().view >= 0) chat_->setSuggestions(turns_.last().view, QString());   // suites : seulement sous la dernière réponse
    turnRuns_.clear();
    Turn t;
    t.question = qShown;
    t.drewModel = drawRequested;
    t.help = helpQuestion;
    for (const SkillMatch& m : matches) t.skillIds << m.skill->id;

    // Calculs déterministes (calibration 14C) : faits par l'application, pas par le modèle.
    // Tableau joint ou collé avec cette question : la consigne le vise (« calcule l'événement « Four » », « date les 14C »).
    const QString qCalc = (attachTable_ || (!tableNotice.isEmpty() && seldon::datatable::hasCurrent() && !tableOnly))
                              ? q + QStringLiteral(" (tableau)") : q;
    // « Combien de temps sépare la date précédente de 14C 3660 ± 25 ? », « quelle durée entre ces deux dates ? » : relation
    // avec les datations des questions précédentes, calculée par SeldonH (sans cela, seule la nouvelle date serait calibrée).
    RunOutcome dating;
    if (!tableOnly && attachName_.isEmpty()) {
        const QVector<QJsonObject> rel = MethodRunner::relationWithPrevious(q, recentDatings());
        if (!rel.isEmpty()) {
            dating = MethodRunner::run(rel, dateformat::detect(q, displayFormat()));
            dating.noticeHtml.prepend(tr("<p><i>↻ Avec la ou les datations des questions précédentes.</i></p>"));
            dating.promptBlock.prepend(QStringLiteral("[La question renvoie aux datations précédentes de la conversation : SeldonH a "
                                                      "fait le calcul avec elles (mêmes mesures, mêmes courbes). Commente ce résultat ; "
                                                      "ne réécris pas de bloc calcul.]\n"));
        }
    }
    if (!dating.found && !tableOnly) dating = MethodRunner::analyzeQuestion(qCalc, dateformat::fromId(settings_.dateFormat));
    // « Et à 50 % ? » : pas de datation dans la question, seulement un autre niveau de confiance -> la dernière datation
    // de la conversation est recalculée à ce niveau (le modèle ne doit pas déduire une HPD des quartiles).
    if (!dating.found) {
        const DateFormat fmt = dateformat::detect(q, displayFormat());
        const QVector<QJsonObject> again = MethodRunner::followUpRequests(q, lastDatingRequests(), fmt);
        if (!again.isEmpty()) {
            dating = MethodRunner::run(again, fmt);
            QStringList what;
            if (const double lv = MethodRunner::requestedLevel(q); lv > 0)
                what << tr("au niveau %1 %").arg(QString::number(lv, 'g', 4));
            if (again.first().contains("tmin"))
                what << tr("sur la période d'étude %1 → %2").arg(dateformat::format(again.first().value("tmin").toDouble(), fmt),
                                                                 dateformat::format(again.first().value("tmax").toDouble(), fmt));
            dating.noticeHtml.prepend(tr("<p><i>↻ Datation précédente recalculée %1.</i></p>").arg(what.join(tr(" et "))));
            dating.promptBlock.prepend(QStringLiteral("[La question demande de refaire la datation précédente %1 : SeldonH l'a "
                                                      "recalculée (mêmes mesures, mêmes courbes). Commente ce résultat ; ne réécris "
                                                      "pas de bloc calcul ; une HPD ne se déduit pas des quartiles.]\n").arg(what.join(" et ")));
        }
    }
    // « Liste les courbes de réf » : la liste complète, affichée par SeldonH (le modèle n'en recopiait qu'une partie).
    if (!dating.found && !tableOnly && attachName_.isEmpty() && !modelRequest && MethodRunner::asksCurveList(qk)) {
        QString plain;
        dating.noticeHtml = MethodRunner::cardMarker(tr("Courbes de référence disponibles")) + curvesListHtml(&plain);
        dating.promptBlock = MethodRunner::shownResultHeader() + plain
                             + QStringLiteral("\nLa liste complète est DÉJÀ affichée : ne la recopie pas ; réponds seulement à une "
                                              "question précise sur ces courbes.\n");
        dating.found = true;
    }
    t.followUp = dating.found;       // déjà calculé par l'application : pas de nouveau calcul à relancer

    QString mu;
    if (dating.found)          mu += dating.promptBlock + "\n";
    if (!knowledge.isEmpty())  mu += "[Connaissances de référence]\n" + knowledge + "\n\n";
    // Tableau courant : son résumé à chaque question (retiré de l'historique avec les connaissances, voir historyUser).
    if (seldon::datatable::hasCurrent()) {
        const int budget = remoteActive() ? 12000 : std::clamp(settings_.nCtx / 3, 2000, 8000);
        mu += seldon::datatable::digest(seldon::datatable::current(), budget) + "\n";
    }
    for (const Attachment& a : std::as_const(attachments_))
        if (!a.text.isEmpty()) mu += "[Fichier joint : " + a.name + "]\n" + a.text + "\n\n";
    // Tableau demandé : SeldonH l'affiche avec les valeurs exactes ; le modèle reçoit ces valeurs (dans un budget) et le
    // commente. Un modèle local à 8 k tokens ne pourrait ni lire ni réécrire 48 phases.
    QString resultsTable;
    if (resultsFocus) {
        if (resultsFocus & chronoresults::FocusTable) resultsTable = chronoresults::tablesHtml(lastResults_, resultsFocus);
        const int budget = remoteActive() ? 16000 : std::clamp(settings_.nCtx / 2, 2500, 12000);
        if (!attachChronoResults_)
            mu += QStringLiteral("[Résultats ChronoModel « %1 », joints plus tôt dans la conversation]\n").arg(lastResultsName_);
        mu += chronoresults::digest(lastResults_, resultsFocus, q, budget, !resultsTable.isEmpty()) + "\n";
    }
    if (drawRequested)
        mu += QStringLiteral("[SeldonH a dessiné le modèle « %1 » dans le panneau des graphiques (scène des Events%2). "
                             "Le dessin est DÉJÀ fait et affiché : n'écris AUCUN bloc de code (ni plot, ni autre) pour le dessiner, "
                             "ne décris pas le dessin ; réponds seulement au reste de la question.]\n\n")
                  .arg(lastChrTitle_, ChrSceneWidget::hasPhases(lastChr_) ? QStringLiteral(" et scène des phases") : QString());
    if (!attachChr_.isEmpty()) { lastChr_ = attachChr_; lastChrTitle_ = attachChrName_; rememberChrDates(attachChr_); }
    const QString asked = tableOnly ? tr("(L'utilisateur a collé ce tableau sans consigne.)") : q;
    if (helpQuestion && !dating.found) mu += MethodRunner::helpHint();
    t.modelUser = mu.isEmpty() ? asked : mu + "[Question]\n" + asked;

    QString shown = qShown;
    if (!attachName_.isEmpty()) shown += "\n\n📎 " + attachName_;
    logUser(shown);
    if (!tableNotice.isEmpty()) logNotice(tableNotice);
    if (attachTable_ && !attachTableHtml_.isEmpty()) logNotice(attachTableHtml_);
    if (!resultsTable.isEmpty()) logNotice(resultsTable);
    if (drawRequested) showChrScenes(lastChr_, lastChrTitle_, true);
    if (dating.found) {
        logNotice(dating.noticeHtml);
        if (!dating.plot.isEmpty()) logDatingPlot(dating.plot, turns_.size());     // le tour t est ajouté juste après
        for (const QJsonObject& p : std::as_const(dating.extraPlots)) logPlot(p);
        applyRunExtras(dating);
    }
    // Mode calculatrice : un calcul que SeldonH a fait seul, sans demande d'explication, est affiché sans réponse du modèle
    // (la fiche donne les valeurs exactes) ; « Demander un commentaire » reste possible.
    // Traces ou tableau joints : lus par SeldonH, le calcul se suffit à lui-même (pas de commentaire automatique).
    const bool calcAlone = (dating.found || tableOnly || !modelReady_) && attachOnlyData() && !resultsFocus
                           && !modelRequest && !drawRequested && (!modelReady_ || !wantsCalcComment(q));
    t.view = newAssistantBubble();
    turns_.append(t);

    input_->clear();
    clearAttachment();
    updateConvTitle();

    saveCurrent();                                // la question est gardée même si l'application s'arrête pendant la réponse
    if (calcAlone) {
        // Tableau seul, ou tableau sans modèle de langage et sans action reconnue : fiche « tableau lu », pas de réponse.
        if (!dating.found)
            finishCalcOnly(modelReady_ ? tr("*Tableau lu par SeldonH : voir ci-dessus. Dites ce que vous voulez en faire : extraire, dater, "
                                            "regrouper en événement, préparer un modèle ChronoModel…*")
                                       : tr("*Tableau lu par SeldonH. Sans modèle de langage, demandez par exemple « extrais les 14C du "
                                            "tableau », « date les TL du tableau » ou « calibre toutes les dates du tableau ».*"));
        else finishCalcOnly();
    }
    else startGeneration();
}

bool MainWindow::wantsCalcComment(const QString& question) const
{
    if (settings_.calcComment >= 2) return true;
    if (settings_.calcComment <= 0) return false;
    return MethodRunner::asksCommentary(question) || MethodRunner::asksCommentary(lexicon_.translate(question));
}

void MainWindow::finishCalcOnly(const QString& shownText)
{
    if (turns_.isEmpty()) return;
    Turn& t = turns_.last();
    t.calcOnly = true;
    t.answer.clear();
    const QString shown = shownText.isEmpty() ? tr("*Résultat calculé par SeldonH : voir la fiche ci-dessus.*") : shownText;
    chat_->setAssistant(t.view, shown, QString(), false, true);
    chat_->setAppNote(t.view, true);
    logAssistantText(turns_.size() - 1, shown, QString());
    computeSuites();
    setTurnActions(turns_.size() - 1);
    generating_ = false;
    continuing_ = false;
    if (toolsApplyPending_) { toolsApplyPending_ = false; applyRemoteTools(false); }
    updateControls();
    statsLabel_->setText(tr("Calcul fait par SeldonH — sans commentaire du modèle"));
    saveCurrent();
}

void MainWindow::requestComment(int turn)
{
    if (turn < 0 || turn >= turns_.size() || !turns_[turn].calcOnly) return;
    if (generating_) { statusBar()->showMessage(tr("Une réponse est en cours : attendez la fin ou arrêtez-la."), 5000); return; }
    if (!modelReady_) {
        statusBar()->showMessage(tr("Aucun modèle prêt : chargez un modèle (ou configurez le modèle distant) pour obtenir un commentaire."), 6000);
        return;
    }
    static const QString ask = QStringLiteral(
        "[L'utilisateur demande un commentaire du résultat de calcul de SeldonH, déjà affiché dans une fiche avec le graphique : "
        "explique ce qu'il signifie et ce qu'il faut en retenir (forme de la densité, plateau ou multimodalité, précision, mises en "
        "garde) sans recopier les valeurs ; ne relance aucun calcul (ni bloc calcul, ni outil).]\n");
    if (turn == turns_.size() - 1) {             // dernier tour : le commentaire remplit sa bulle
        Turn& t = turns_[turn];
        t.calcOnly = false;
        t.followUp = true;
        t.modelUser = ask + t.modelUser;
        chat_->setActions(t.view, {});
        chat_->setAssistant(t.view, QString(), QString(), true, false);
        logAssistantText(turn, QString(), QString());
    } else {                                     // résultat plus ancien : nouveau tour en bas de la conversation
        QString about;
        for (int i = turn; i >= 0 && about.isEmpty(); --i)
            if (!turns_[i].synthetic) about = turns_[i].question;
        Turn t;
        t.question = tr("(commentaire d'un résultat de SeldonH)");
        t.modelUser = ask + QStringLiteral("[Résultat concerné : celui de la question « %1 », plus haut dans la conversation.]").arg(about);
        t.followUp = true;
        t.synthetic = true;
        t.view = newAssistantBubble();
        turns_.append(t);
    }
    startGeneration();
}

void MainWindow::startGeneration()
{
    raw_.clear();
    calcCut_ = false;
    const bool thinking = settings_.thinking && thinkingSupported();     // sans effet sur un modèle qui ne sait pas réfléchir
    startsInThink_ = thinking;                // corrigé par le signal `started`
    generating_ = true;
    updateControls();
    statsLabel_->setText(tr("Évaluation du prompt…"));

    SamplingParams sp;
    sp.temperature    = static_cast<float>(settings_.temperature);
    sp.maxTokens      = settings_.maxTokens + (thinking ? AppSettings::kThinkingReserve : 0);   // + réserve pour la réflexion
    sp.enableThinking = thinking;
    QJsonArray refs;
    const QVector<ChatMessage> chat = buildChat(&refs);
    if (!turns_.isEmpty()) turns_.last().prompt = promptSnapshot(refs, sp);
    if (remote_) remote_->setToolsSuspended(helpTurn());           // question d'aide : pas d'appel d'outil
    if (remoteActive()) emit requestRemoteGenerate(chat, sp);
    else emit requestGenerate(chat, sp);
}

bool MainWindow::helpTurn() const
{
    const int k = currentQuestionTurn();
    return k >= 0 && turns_[k].help;
}

// Suite d'une réponse coupée : le modèle reçoit la conversation, le début de sa réponse et la consigne de continuer ;
// le texte produit s'ajoute à la même bulle et au même tour (un bloc ```chronomodel coupé redevient complet).
void MainWindow::continueAnswer(int turn)
{
    if (generating_ || turns_.isEmpty()) return;
    if (turn != turns_.size() - 1) {
        statusBar()->showMessage(tr("Seule la dernière réponse peut être continuée."), 5000);
        return;
    }
    if (!modelReady_) {
        statusBar()->showMessage(tr("Aucun modèle prêt : impossible de continuer la réponse."), 5000);
        return;
    }
    Turn& t = turns_.last();
    if (!t.truncation || t.answer.isEmpty()) return;
    t.truncation = 0;
    chat_->setActions(t.view, {});

    QJsonArray refs;
    QVector<ChatMessage> chat = buildChat(&refs);  // se termine par la question de ce tour
    chat.append(ChatMessage{ "assistant", t.answer.toStdString() });
    chat.append(ChatMessage{ "user", "[SeldonH] Ta réponse précédente a été coupée avant la fin. Écris uniquement la suite, en reprenant "
                          "exactement là où le texte s'arrête, éventuellement au milieu d'un mot, d'une phrase ou d'un bloc de "
                          "code (dans ce cas, ne rouvre pas le bloc). Ne répète rien de ce qui est déjà écrit, ne résume pas, "
                          "ne t'excuse pas." });

    continuing_ = true;
    raw_ = t.answer;                              // la suite s'ajoute à la réponse déjà affichée
    startsInThink_ = false;
    generating_ = true;
    updateControls();
    statsLabel_->setText(tr("Suite de la réponse…"));

    SamplingParams sp;
    sp.temperature    = static_cast<float>(settings_.temperature);
    sp.maxTokens      = settings_.maxTokens;
    sp.enableThinking = false;                    // pas de nouvelle réflexion : seulement la suite du texte
    for (int k = int(refs.size()); k < chat.size(); ++k)       // début de réponse et consigne de suite : texte tel quel
        refs.append(QJsonObject{ { "role", QString::fromStdString(chat[k].role) }, { "content", QString::fromStdString(chat[k].content) } });
    {
        QJsonArray suites = t.prompt.value("suites").toArray();
        suites.append(promptSnapshot(refs, sp));
        t.prompt["suites"] = suites;
    }
    if (remoteActive()) emit requestRemoteGenerate(chat, sp);
    else emit requestGenerate(chat, sp);
}

// Le bloc ```chronomodel n'est pas montré tel quel : un résumé (modèle prêt / ce qui manque) le remplace, sauf si la
// dernière vraie question de l'utilisateur demande explicitement le JSON.
QString MainWindow::calcHtml(const QVector<Calculator::Line>& lines) const
{
    QString h = MethodRunner::cardMarker(tr("Calculatrice (sans le modèle de langage)")) + QStringLiteral("<table cellspacing='0' cellpadding='3'>");
    for (const Calculator::Line& l : lines) {
        h += QStringLiteral("<tr><td><code>%1</code></td><td>").arg(l.expr.toHtmlEscaped());
        const QString shown = Calculator::formatValue(l.value, l.uncertainty);
        if (l.ok) h += l.variable.isEmpty() ? QStringLiteral("= <b>%1</b>").arg(shown)
                                            : QStringLiteral("→ <b>%1</b> = %2").arg(l.variable.toHtmlEscaped(), shown);
        else h += QStringLiteral("⚠ %1").arg(l.error.toHtmlEscaped());
        if (l.ok && !l.details.isEmpty()) {
            h += QStringLiteral("<br><i>%1</i>").arg(l.details.toHtmlEscaped());
            if (!l.variable.isEmpty() && l.parts.size() > 1) {
                QStringList names;
                for (int k = 1; k < l.parts.size(); ++k) names << l.variable + "_" + l.parts[k].first;
                h += tr("<br><small>variables : %1</small>").arg(names.join(", ").toHtmlEscaped());
            }
        }
        h += QStringLiteral("</td></tr>");
    }
    h += QStringLiteral("</table>");
    bool withErr = false;
    for (const Calculator::Line& l : lines) withErr = withErr || (l.ok && l.uncertainty > 0);
    if (withErr)
        h += tr("<p><small>Incertitudes (1σ) propagées au premier ordre en supposant les <b>erreurs indépendantes</b> : "
                "une même valeur utilisée deux fois n'est pas reconnue comme corrélée (a − a donne 0 ± σ√2).</small></p>");
    return h;
}

// Un bloc ```chronomodel n'est admis que si la dernière vraie question demande un modèle (ou son JSON), ou si la
// conversation en a déjà produit un (« ajoute une phase » à la suite). Sinon (simple datation), le modèle de langage
// l'a ajouté de lui-même : il est retiré, au lieu d'afficher « Modèle ChronoModel incomplet ».
bool MainWindow::modelExpected() const
{
    int last = int(turns_.size()) - 1;
    while (last >= 0 && turns_[last].synthetic) --last;
    if (last < 0) return true;
    const QString& q = turns_[last].question;
    if (MethodRunner::isModelRequest(q) || ChrBuilder::asksForJson(q)) return true;
    QJsonObject spec;
    for (int i = 0; i < last; ++i)
        if (ChrBuilder::extractSpec(turns_[i].answer, spec)) return true;
    return false;
}

QString MainWindow::shownAnswer(const QString& answer) const
{
    if (!modelExpected())
        return ChrBuilder::displayMarkdown(MethodRunner::hideCalcBlocks(ChrBuilder::stripModelBlocks(answer), false), false);
    bool showJson = false;
    for (int i = turns_.size() - 1; i >= 0; --i)
        if (!turns_[i].synthetic) { showJson = ChrBuilder::asksForJson(turns_[i].question); break; }
    // Blocs de calcul et JSON de calcul : jamais montrés tels quels (l'utilisateur n'a pas à écrire de requête).
    return ChrBuilder::displayMarkdown(MethodRunner::hideCalcBlocks(answer, showJson), showJson);
}

QString MainWindow::truncationNotice(const Turn& t) const
{
    switch (t.truncation) {
    case GenStats::TokenLimit:
        return tr("⚠ Réponse incomplète : limite de %1 tokens atteinte. Cliquez sur « Continuer la réponse », ou augmentez "
                  "« Longueur max. d'une réponse » dans les Préférences.").arg(t.tokenLimit);
    case GenStats::ContextFull:
        return tr("⚠ Réponse incomplète : plus de place dans le contexte (%1 tokens pour la réponse). Cliquez sur « Continuer "
                  "la réponse », augmentez la taille du contexte ou commencez une nouvelle conversation.").arg(t.tokenLimit);
    case GenStats::StreamCut:
        return t.truncationDetail.isEmpty()
                   ? tr("⚠ Réponse incomplète : la connexion au modèle distant s'est interrompue. Cliquez sur « Continuer la réponse ».")
                   : tr("⚠ Réponse incomplète : le modèle distant a signalé une erreur (%1). Cliquez sur « Continuer la réponse ».")
                         .arg(t.truncationDetail);
    default:
        return {};
    }
}

// Dernières datations de la conversation (au plus 8 tours en arrière) : écrites dans une question de l'utilisateur,
// ou demandées par l'assistant dans un bloc « calcul » (ou un appel d'outil, ré-écrit en bloc).
QVector<QJsonObject> MainWindow::recentDatings() const
{
    // Datations écrites par l'utilisateur dans les 8 dernières questions (et termes datés des relations), sans doublon.
    const MethodRegistry& reg = MethodRegistry::instance();
    QVector<QJsonObject> out;
    QStringList seen;
    auto add = [&](QJsonObject o) {
        o = MethodRunner::normalizeRequest(o);
        if (!reg.byId(o.value("method").toString())) return;
        for (const char* k : { "level", "layout" }) o.remove(k);
        const QString key = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
        if (seen.contains(key)) return;
        seen << key;
        out << o;
    };
    for (int i = std::max(0, int(turns_.size()) - 8); i < turns_.size(); ++i) {
        if (turns_[i].synthetic) continue;
        for (const QJsonObject& r : MethodRunner::requestsFromQuestion(turns_[i].question)) {
            if (r.value("method").toString() == QLatin1String("relation")) {
                // Termes de la relation : datations, et colonnes des traces jointes (« Pomigliano del Arco », « Phase A End »),
                // pour « quelle est la durée entre ces deux dates ? » après une question sur deux colonnes.
                QJsonArray terms;
                for (const char* k : { "of", "from", "to" }) if (r.contains(k)) terms.append(r.value(k));
                for (const QJsonValue& v : r.value("terms").toArray()) terms.append(v);
                for (const QJsonValue& v : std::as_const(terms)) {
                    if (v.isObject()) add(v.toObject());
                    else if (v.isString() && !v.toString().trimmed().isEmpty()) {
                        const QString key = QStringLiteral("column:") + v.toString();
                        if (!seen.contains(key)) { seen << key; out << QJsonObject{ { "_column", v.toString() } }; }
                    }
                }
            } else {
                add(r);
            }
        }
    }
    return out;
}

QVector<QJsonObject> MainWindow::lastDatingRequests() const
{
    // D'abord les datations écrites par l'utilisateur (valeurs et courbes exactes) ; à défaut, un bloc « calcul » de
    // l'assistant (il peut avoir perdu une courbe ou ajouté une clé).
    for (int i = turns_.size() - 1, n = 0; i >= 0 && n < 8; --i, ++n)
        if (!turns_[i].synthetic)
            if (const QVector<QJsonObject> r = MethodRunner::requestsFromQuestion(turns_[i].question); !r.isEmpty()) return r;
    for (int i = turns_.size() - 1, n = 0; i >= 0 && n < 8; --i, ++n)
        if (const QVector<QJsonObject> b = MethodRunner::extractCalcBlocks(turns_[i].answer + turns_[i].toolBlocks); !b.isEmpty()) return b;
    return {};
}

// Format : celui des Préférences, ou celui demandé dans la dernière vraie question de l'utilisateur.
DateFormat MainWindow::displayFormat() const
{
    DateFormat fmt = dateformat::fromId(settings_.dateFormat);
    for (int i = turns_.size() - 1; i >= 0; --i)
        if (!turns_[i].followUp || turns_[i].question != tr("(résultat du calcul demandé par l'assistant)")) {
            fmt = dateformat::detect(turns_[i].question, fmt);
            break;
        }
    return fmt;
}

// L'assistant peut demander un calcul avec un bloc « calcul » (JSON). L'application l'exécute (méthodes de
// ChronoModel), affiche la fiche et la courbe, puis relance l'assistant avec les valeurs exactes pour qu'il les
// commente. Un seul tour de suite par question : pas de boucle.
bool MainWindow::runRequestedCalculations()
{
    if (turns_.isEmpty() || turns_.last().followUp) return false;
    if (helpTurn()) return false;                // réponse d'aide : un bloc n'y serait qu'un exemple, jamais exécuté
    QVector<QJsonObject> blocks = MethodRunner::extractCalcBlocks(turns_.last().answer);
    if (blocks.isEmpty()) return false;
    for (QJsonObject& b : blocks) b = MethodRunner::fillDateNames(b, chrDates_);

    const RunOutcome out = MethodRunner::run(blocks, displayFormat());
    if (!out.found) return false;
    logNotice(out.noticeHtml);
    if (!out.plot.isEmpty()) logDatingPlot(out.plot, currentQuestionTurn());
    for (const QJsonObject& p : out.extraPlots) logPlot(p);
    applyRunExtras(out);

    const bool comment = wantsCalcComment(turns_[currentQuestionTurn()].question);
    Turn t;
    t.question = tr("(résultat du calcul demandé par l'assistant)");
    t.modelUser = out.promptBlock;
    t.followUp = true;
    t.synthetic = true;
    t.view = newAssistantBubble();
    turns_.append(t);
    if (comment) startGeneration();
    else finishCalcOnly();                       // mode calculatrice : la fiche suffit
    return true;
}

void MainWindow::stopGeneration()
{
    if (remoteActive()) { if (remote_) remote_->stop(); return; }
    if (engine_) engine_->stop();
}

void MainWindow::onToken(const QString& piece)
{
    if (turns_.isEmpty()) return;
    raw_ += piece;
    const SplitText s = splitThinking(raw_.toStdString(), startsInThink_);
    // Demande de calcul terminée (bloc ```calcul) et le modèle continue à écrire : la suite serait un résultat inventé
    // (il ne connaît pas encore les valeurs). SeldonH arrête la génération, fait le calcul et affiche la fiche ; le modèle
    // commente ensuite les vraies valeurs si la question le demande. Plusieurs blocs à la suite restent possibles.
    if (!calcCut_ && !continuing_ && !turns_.last().followUp && !helpTurn()) {
        const QString a = QString::fromStdString(s.answer);
        qsizetype b0 = -1, b1 = -1;
        if (MethodRunner::lastCalcBlock(a, &b0, &b1)) {
            const QString tail = a.mid(b1).trimmed();
            if (tail.size() >= 12 && !tail.startsWith(QLatin1Char('`')) && !tail.startsWith(QLatin1Char('{'))) {
                calcCut_ = true;
                stopGeneration();
            }
        }
    }
    const QString thinking = continuing_ ? turns_.last().thinking : QString::fromStdString(s.thinking);
    chat_->setAssistant(turns_.last().view, shownAnswer(QString::fromStdString(s.answer)), thinking, !continuing_ && s.thinkingOpen, false);
}

void MainWindow::finishAssistantMessage(bool interrupted)
{
    if (turns_.isEmpty()) return;
    Turn& t = turns_.last();
    const SplitText s = splitThinking(raw_.toStdString(), startsInThink_);
    t.answer = QString::fromStdString(s.answer);       // texte complet (avec le bloc de modèle) : historique, enregistrement
    if (!modelExpected()) t.answer = ChrBuilder::stripModelBlocks(t.answer);     // bloc de modèle non demandé
    if (helpTurn()) t.answer = MethodRunner::stripCodeBlocks(t.answer);          // aide : phrases à taper, pas de code
    qsizetype b0 = -1, b1 = -1;
    const bool endsWithCalc = !t.followUp && !helpTurn() && MethodRunner::lastCalcBlock(t.answer, &b0, &b1)
                              && (calcCut_ || t.answer.mid(b1).trimmed().isEmpty());
    if (endsWithCalc) t.answer.truncate(b1);           // texte écrit après la demande de calcul : inventé, retiré
    QString answer = shownAnswer(t.answer);
    // Réponse réduite à une demande de calcul (titres, « je calcule… ») : simple note ; la fiche de SeldonH suit.
    bool calcRequestNote = false;
    if (endsWithCalc && !interrupted) {
        QString rest = answer;
        rest.remove(QStringLiteral("*Calcul confié à SeldonH.*"));
        static const QRegularExpression headings(QStringLiteral("^\\s*#+[^\\n]*$"), QRegularExpression::MultilineOption);
        rest.remove(headings);
        if (rest.trimmed().size() < 300) { answer = calcRequestText(); calcRequestNote = true; }
    }
    if (t.drewModel) {                                 // dessin fait par SeldonH : un bloc plot du modèle serait faux
        answer = ChrSceneWidget::stripPlotBlocks(answer);
        if (answer.isEmpty()) answer = tr("*Modèle dessiné par SeldonH dans le panneau des graphiques.*");
    }
    if (!continuing_) t.thinking = QString::fromStdString(s.thinking);
    if (interrupted) answer += tr("\n\n*(réponse interrompue)*");
    else if (t.truncation) answer += QStringLiteral("\n\n*%1*").arg(truncationNotice(t));
    chat_->setAssistant(t.view, answer, t.thinking, false, true);
    if (calcRequestNote) chat_->setAppNote(t.view, true);
    logAssistantText(turns_.size() - 1, answer, t.thinking);
    computeSuites();
    setTurnActions(turns_.size() - 1);

    if (!t.drewModel) applyPlotBlocks(t.answer);

    // Modèle ChronoModel complet : il est dessiné tout de suite (scène des Events, et des phases s'il y en a).
    QJsonObject spec;
    if (!interrupted && ChrBuilder::extractSpec(t.answer, spec)) {
        const ChrResult r = ChrBuilder::build(spec);
        // Vérification croisée avec ce qu'a écrit l'utilisateur : valeurs ou références inventées, données oubliées.
        const QStringList gaps = ChrBuilder::crossCheck(spec, userSourceText());
        if (!gaps.isEmpty()) {
            QString h = tr("<p><b>⚠ Vérification du modèle par rapport à votre texte</b></p><ul>");
            for (const QString& g : gaps) h += QStringLiteral("<li>%1</li>").arg(g.toHtmlEscaped());
            h += tr("</ul><p><i>Vérifiez ces points avant d'enregistrer ; demandez à l'assistant de corriger le modèle si besoin.</i></p>");
            logNotice(h);
        }
        if (r.ok) {
            lastChr_ = r.chr;
            lastChrTitle_ = r.suggestedName;
            showChrScenes(r.chr, r.suggestedName, true);
        }
    }
}

void MainWindow::showChrScene(const QJsonObject& chr, const QString& kind, const QString& title)
{
    QPointer<ChrSceneWidget>& page = kind == QLatin1String("phases") ? phaseScene_ : eventScene_;
    if (page) page->setChr(chr);
    else page = new ChrSceneWidget(chr, kind == QLatin1String("phases") ? ChrSceneWidget::Phases : ChrSceneWidget::Events);
    showFiguresPanel();
    figures_->showPage(page, title);
}

void MainWindow::addSceneFigure(const QJsonObject& fig)
{
    const QJsonObject chr = fig.value("chr").toObject();
    const auto kind = fig.value("chr_scene").toString() == QLatin1String("phases") ? ChrSceneWidget::Phases : ChrSceneWidget::Events;
    const int id = chatFigures_.size();
    chatFigures_.append(ChatFigure{ fig, nullptr });
    chat_->addFigure(id, ChrSceneWidget::thumbnail(chr, kind, 520, devicePixelRatioF()), fig.value("title").toString());
}

QString MainWindow::userSourceText() const
{
    QString s;
    for (const Turn& t : turns_) {
        if (t.synthetic) continue;
        s += t.question + QLatin1Char('\n');
        const int a = t.modelUser.indexOf(QStringLiteral("[Fichier joint : "));
        if (a >= 0) {
            const int b = t.modelUser.indexOf(QStringLiteral("\n[Question]"), a);
            s += t.modelUser.mid(a, b < 0 ? -1 : b - a) + QLatin1Char('\n');
        }
    }
    return s;
}

void MainWindow::showChrScenes(const QJsonObject& chr, const QString& name, bool inChat)
{
    const QString base = QFileInfo(name).completeBaseName();
    const QString suffix = base.isEmpty() ? QString() : QStringLiteral(" — ") + base;
    const bool phases = ChrSceneWidget::hasPhases(chr);
    if (phases) showChrScene(chr, QStringLiteral("phases"), tr("Scène des phases") + suffix);
    else if (phaseScene_) delete phaseScene_;      // modèle sans phase : l'ancienne scène des phases n'a plus de sens
    showChrScene(chr, QStringLiteral("events"), tr("Scène des Events") + suffix);
    if (!inChat) return;

    const int nEv = chr.value("events").toArray().size(), nCons = chr.value("events_constraints").toArray().size();
    QString html = tr("🗺 Modèle%1 dessiné dans le panneau des graphiques : <b>scène des Events</b> (%2 événement(s), %3 contrainte(s))")
                       .arg(base.isEmpty() ? QString() : QStringLiteral(" « %1 »").arg(base.toHtmlEscaped())).arg(nEv).arg(nCons);
    if (phases)
        html += tr(" et <b>scène des phases</b> (%1 phase(s), %2 contrainte(s))")
                    .arg(chr.value("phases").toArray().size()).arg(chr.value("phases_constraints").toArray().size());
    html += tr(". Cliquez sur une vignette pour l'ouvrir ; dans le panneau : molette pour zoomer, glisser pour déplacer, "
               "double-clic pour la vue d'ensemble, <i>Enregistrer…</i> pour l'export PNG ou SVG.");
    logNotice(html);
    // Vignettes cliquables, gardées dans l'historique (élément « plot » portant le .chr).
    QStringList kinds{ QStringLiteral("events") };
    if (phases) kinds << QStringLiteral("phases");
    for (const QString& k : std::as_const(kinds)) {
        const QJsonObject fig{ { "chr_scene", k }, { "chr", chr },
                               { "title", (k == QLatin1String("phases") ? tr("Scène des phases") : tr("Scène des Events")) + suffix } };
        addSceneFigure(fig);
        DisplayItem d; d.kind = DisplayItem::Plot; d.plot = fig;
        display_.append(d);
    }
}

void MainWindow::onFinished(const GenStats& st)
{
    generating_ = false;
    if (!turns_.isEmpty()) {
        Turn& t = turns_.last();
        if (st.droppedMessages > 0) t.prompt["dropped_messages"] = st.droppedMessages;   // ce que le moteur a réellement gardé
        t.truncation = st.stoppedByUser ? 0 : st.truncation;      // (arrêt après un bloc de calcul : pas de troncature)
        t.tokenLimit = st.tokenLimit;
        t.truncationDetail = QString::fromStdString(st.truncationDetail);
    }
    const bool stopped = st.stoppedByUser && !calcCut_;      // arrêt par SeldonH après un bloc de calcul : pas une interruption
    finishAssistantMessage(stopped);
    calcCut_ = false;
    continuing_ = false;
    if (toolsApplyPending_) { toolsApplyPending_ = false; applyRemoteTools(false); }
    saveCurrent();

    const double tps = st.genMs > 0 ? st.generated * 1000.0 / st.genMs : 0.0;
    if (!remoteActive() && st.generated >= 32) lastLocalTps_ = tps;     // pour « Optimiser pour ce modèle »
    QString msg = tr("%1 tokens générés — %2 tok/s").arg(st.generated).arg(tps, 0, 'f', 1);
    if (st.droppedMessages > 0)
        msg += tr(" — %1 ancien(s) message(s) oublié(s) (contexte plein)").arg(st.droppedMessages);
    if (st.contextFull) msg += tr(" — contexte saturé");
    if (!stopped && st.truncation) msg += tr(" — réponse coupée");

    updateQuota(st);
    if (!stopped && runRequestedCalculations()) return;      // relance l'assistant avec le résultat
    updateControls();
    statsLabel_->setText(msg);
}

// Place restante pour la conversation : contexte du modèle moins le prompt de la dernière question et sa réponse
// (local : contexte créé ; distant : contexte annoncé par /models). Distant : quota du fournisseur s'il est annoncé.
void MainWindow::updateQuota(const GenStats& st)
{
    if (!quotaLabel_) return;
    const QLocale loc;
    const int cap = remoteActive() ? remoteCtx_ : localCtx_;
    const int used = st.promptTokens >= 0 ? st.promptTokens + std::max(0, st.generated) : -1;
    QString text, tip;
    if (cap > 0 && used >= 0) {
        const int left = std::max(0, cap - used);
        const int pct = int(std::lround(100.0 * left / cap));
        const QString color = pct < 10 ? QStringLiteral("#c0392b") : pct < 25 ? QStringLiteral("#d68910") : QString();
        const QString body = tr("Contexte libre : %1 % (%2 sur %3 tokens)").arg(pct).arg(loc.toString(left), loc.toString(cap));
        text = color.isEmpty() ? body.toHtmlEscaped() : QStringLiteral("<span style='color:%1'>%2</span>").arg(color, body.toHtmlEscaped());
        tip = tr("Place restante dans le contexte du modèle pour cette conversation (prompt de la dernière question et sa réponse : "
                 "%1 tokens). Quand il est plein, %2").arg(loc.toString(used),
                 remoteActive() ? tr("le serveur refuse la question ou coupe la réponse : ouvrez une nouvelle conversation.")
                                : tr("SeldonH oublie les plus anciens échanges."));
    } else if (used >= 0) {
        text = tr("Conversation : %1 tokens (contexte du modèle inconnu)").arg(loc.toString(used)).toHtmlEscaped();
        tip = tr("Le serveur n'annonce pas la taille du contexte de ce modèle dans sa liste /models.");
    }
    if (remoteActive() && (st.rateTokensRemaining >= 0 || st.rateRequestsRemaining >= 0)) {
        QStringList q;
        if (st.rateTokensRemaining >= 0)
            q << (st.rateTokensLimit > 0 ? tr("%1 / %2 tokens").arg(loc.toString(st.rateTokensRemaining), loc.toString(st.rateTokensLimit))
                                         : tr("%1 tokens").arg(loc.toString(st.rateTokensRemaining)));
        if (st.rateRequestsRemaining >= 0)
            q << (st.rateRequestsLimit > 0 ? tr("%1 / %2 requêtes").arg(loc.toString(st.rateRequestsRemaining), loc.toString(st.rateRequestsLimit))
                                           : tr("%1 requêtes").arg(loc.toString(st.rateRequestsRemaining)));
        if (!text.isEmpty()) text += QStringLiteral(" — ");
        text += tr("Quota restant : %1").arg(q.join(QStringLiteral(", "))).toHtmlEscaped();
        tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) +
               tr("Quota du fournisseur, lu dans les en-têtes de sa dernière réponse (souvent par minute ou par jour).");
    }
    quotaLabel_->setText(text);
    quotaLabel_->setToolTip(tip);
}

void MainWindow::resetQuota()
{
    if (!quotaLabel_) return;
    const int cap = remoteActive() ? remoteCtx_ : localCtx_;
    quotaLabel_->setText(cap > 0 ? tr("Contexte libre : 100 % (%1 tokens)").arg(QLocale().toString(cap)).toHtmlEscaped() : QString());
    quotaLabel_->setToolTip(cap > 0 ? tr("Place disponible dans le contexte du modèle pour cette conversation.") : QString());
}

// Contexte du modèle distant, annoncé par GET {Base URL}/models (OpenRouter, Mistral, vLLM…) ; inconnu sinon.
void MainWindow::fetchRemoteContext()
{
    if (!limitsNam_) limitsNam_ = new QNetworkAccessManager(this);
    const QString base = settings_.remoteUrl, model = settings_.remoteModel;
    QNetworkRequest rq(remote::modelsUrl(base));
    if (!settings_.remoteKey.isEmpty()) rq.setRawHeader("Authorization", "Bearer " + settings_.remoteKey.toUtf8());
    rq.setTransferTimeout(15000);
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply* r = limitsNam_->get(rq);
    connect(r, &QNetworkReply::finished, this, [this, r, base, model] {
        r->deleteLater();
        if (!remoteActive() || settings_.remoteUrl != base || settings_.remoteModel != model) return;   // moteur changé entre-temps
        const QByteArray body = r->isOpen() ? r->readAll() : QByteArray();
        remoteCtx_ = remote::parseModelLimits(body).value(model).context;
        if (turns_.isEmpty()) resetQuota();
    });
}

// Modèle distant : appel d'outils natif. Mêmes calculs que le bloc ```calcul (MethodRegistry), exécutés ici, sur cet
// ordinateur ; seul le texte du résultat repart vers le serveur. Le tour est ensuite réécrit en bloc ```calcul dans
// l'historique, de sorte que la conversation reste lisible par n'importe quel moteur.
void MainWindow::onToolCalls(const QVector<ToolCall>& calls, const QString& /*text*/, const GenStats& /*st*/)
{
    if (turns_.isEmpty()) { generating_ = false; updateControls(); return; }
    finishAssistantMessage(false);                      // texte écrit avant les appels (souvent vide)
    continuing_ = false;

    QStringList called;
    for (const ToolCall& c : calls) called << "`" + c.name + "`";
    if (turns_.last().answer.trimmed().isEmpty()) {
        const QString shown = tr("*🔧 Outil de calcul appelé : %1*").arg(called.join(", "));
        chat_->setAssistant(turns_.last().view, shown, QString(), false, true);
        logAssistantText(turns_.size() - 1, shown, QString());
    }

    const DateFormat fmt = displayFormat();
    QVector<ToolResult> results;
    QVector<QJsonObject> plots, extra;
    QStringList forModel;
    QString blocks;
    bool allExecuted = !calls.isEmpty();
    for (const ToolCall& c : calls) {
        QString args = c.arguments;
        if (const QJsonDocument a = QJsonDocument::fromJson(args.toUtf8()); a.isObject() && !chrDates_.isEmpty())
            args = QString::fromUtf8(QJsonDocument(MethodRunner::fillDateNames(a.object(), chrDates_)).toJson(QJsonDocument::Compact));
        const tools::ToolOutcome o = tools::execute(c.name, args, fmt);
        results.append({ c.id, c.name, o.content });
        if (o.executed) {
            logNotice(o.run.noticeHtml);
            applyRunExtras(o.run);
            plots << o.run.plot;
            extra += o.run.extraPlots;
            blocks += QStringLiteral("\n\n```calcul\n%1\n```").arg(QString::fromUtf8(QJsonDocument(o.request).toJson(QJsonDocument::Compact)));
            forModel << o.content;
        } else {
            allExecuted = false;                 // valeur manquante, erreur : le modèle doit réagir (demander, corriger)
            forModel << tr("[Retour de l'outil « %1 » (SeldonH)]\n%2").arg(c.name, o.content);
            if (c.name != QLatin1String("courbes_disponibles"))
                logNotice(tr("<i>L'assistant a appelé l'outil <code>%1</code> mais le calcul n'a pas pu être lancé : %2</i>")
                                     .arg(c.name.toHtmlEscaped(), o.content.toHtmlEscaped()));
        }
    }
    const QJsonObject plot = tools::mergePlots(plots);
    if (!plot.isEmpty()) logDatingPlot(plot, currentQuestionTurn());
    for (const QJsonObject& p : std::as_const(extra)) logPlot(p);
    turns_.last().toolBlocks = blocks;

    Turn t;
    t.question = tr("(résultat du calcul demandé par l'assistant)");
    t.modelUser = forModel.join("\n\n");
    t.followUp = true;
    t.synthetic = true;
    t.view = newAssistantBubble();
    const bool comment = !allExecuted || wantsCalcComment(turns_[currentQuestionTurn()].question);
    turns_.append(t);
    if (!comment) { finishCalcOnly(); return; }   // mode calculatrice : rien n'est renvoyé au serveur

    raw_.clear();
    startsInThink_ = false;
    statsLabel_->setText(tr("Calcul SeldonH terminé — le modèle rédige la réponse…"));
    remote_->submitToolResults(results);
}

void MainWindow::onFailed(const QString& error)
{
    generating_ = false;
    finishAssistantMessage(false);
    continuing_ = false;
    logNotice(tr("<b>Erreur :</b> %1").arg(error.toHtmlEscaped()));
    saveCurrent();
    updateControls();
}

void MainWindow::newConversation()
{
    if (generating_) return;
    saveCurrent();                       // la conversation en cours reste dans l'historique
    resetConversation(false);
    showWelcome();
    history_->setItems(historyItems_, convId_);
}

void MainWindow::resetConversation(bool clearFigures)
{
    turns_.clear();
    display_.clear();
    datingPlotTurn_ = datingPlotItem_ = datingPlotFig_ = -1;
    datingPlotPage_ = nullptr;
    chatFigures_.clear();
    convId_.clear();
    convCreated_ = QDateTime();
    lastChr_ = QJsonObject();
    chrDates_.clear();
    lastChrTitle_.clear();
    lastResults_ = chronoresults::Report();
    lastResultsName_.clear();
    seldon::traces::clear();             // traces MCMC jointes : propres à la conversation
    seldon::curves::clear();             // courbes de ChronoModel jointes : idem
    seldon::datatable::clear();          // tableau de datations : idem
    CurveStore::clearSession();          // courbes de référence jointes (plugin Gauss) : idem
    resetQuota();                        // nouvelle conversation : tout le contexte est libre
    calc_.reset();
    chat_->clearAll();
    updateConvTitle();
    if (clearFigures) figures_->clearCurves();
    emit requestReset();
    clearAttachment();
    statsLabel_->clear();
}

// ------------------------------------------------------------ pièces jointes, liens

void MainWindow::attachFile()
{
    // Sélection multiple (traces des Events et des phases, splines et Chain_all_Events.csv, journaux de ChronoModel…) ;
    // un nouveau clic sur 📎 ajoute d'autres fichiers à ceux déjà joints.
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Joindre un ou plusieurs fichiers"), QString(),
        tr("Fichiers pris en charge (*.chr *.json *.html *.htm *.csv *.tsv *.txt *.md *.xlsx *.xlsm *.ods);;"
           "Tableaux (*.xlsx *.xlsm *.ods *.csv *.tsv *.txt);;Résultats ChronoModel (*.html *.htm);;Tous les fichiers (*)"));
    for (const QString& f : files) attachOne(f);
}

bool MainWindow::attachOnlyData() const
{
    for (const Attachment& a : attachments_) if (!a.data) return false;
    return true;
}

void MainWindow::updateAttachLabel()
{
    QStringList names, tips;
    qsizetype chars = 0;
    for (const Attachment& a : std::as_const(attachments_)) {
        names << a.name;
        tips << tr("%1 (%2 caractères%3)").arg(a.name).arg(a.text.size()).arg(a.data ? tr(", lu par SeldonH") : QString());
        chars += a.text.size();
    }
    attachName_ = names.join(QStringLiteral(", "));
    if (attachments_.isEmpty()) { attachLabel_->hide(); attachClear_->hide(); return; }
    const QString list = attachLabel_->fontMetrics().elidedText(attachName_, Qt::ElideRight, 520);
    attachLabel_->setText(attachments_.size() == 1 ? tr("📎 %1 (%2 caractères)").arg(list).arg(chars)
                                                   : tr("📎 %1 fichiers : %2 (%3 caractères)").arg(attachments_.size()).arg(list).arg(chars));
    attachLabel_->setToolTip(tips.join('\n'));
    attachClear_->setToolTip(attachments_.size() == 1 ? tr("Retirer la pièce jointe") : tr("Retirer les %1 pièces jointes").arg(attachments_.size()));
    attachLabel_->show();
    attachClear_->show();
}

void MainWindow::attachOne(const QString& f)
{

    const QString ext = QFileInfo(f).suffix().toLower();
    if (QStringList{ "png", "jpg", "jpeg", "svg", "bmp", "gif", "webp", "tif", "tiff", "heic" }.contains(ext)) {
        // Le modèle ne reçoit que du texte : une image jointe ne serait pas vue (risque de réponse inventée).
        QMessageBox::information(this, tr("Joindre un fichier"),
                                 tr("Les images ne sont pas prises en charge : le modèle de langage ne reçoit que du texte.\n"
                                    "Joignez plutôt un tableau (CSV, TXT), un projet ChronoModel (.chr) ou ses résultats (.html)."));
        return;
    }
    if (ext == "xls") {
        QMessageBox::information(this, tr("Joindre un fichier"),
                                 tr("Ancien format Excel (.xls) : enregistrez le classeur au format .xlsx (ou .ods, .csv), puis joignez-le."));
        return;
    }
    QFile file(f);
    if (!file.open(QIODevice::ReadOnly)) { QMessageBox::warning(this, tr("Fichier"), file.errorString()); return; }
    const QByteArray data = file.readAll();

    QString text;
    QString suggestions;             // questions proposées d'après le contenu du fichier (noms réels des Events, phases…)
    bool chronoResults = false;
    bool dataRead = false;           // ce fichier est lu par SeldonH (tableau, traces, courbe) : le modèle n'en reçoit qu'un résumé
    // Tableau de datations (classeur, CSV) : lu par SeldonH, gardé comme tableau courant ; le modèle n'en reçoit que le résumé.
    auto useTable = [&](const seldon::datatable::Table& t) {
        seldon::datatable::setCurrent(t);
        attachTable_ = true;
        dataRead = true;
        attachTableHtml_ = seldon::datatable::summaryHtml(t);
        text = tr("[tableau lu par SeldonH : %1 ligne(s), %2 datable(s)]").arg(t.cells.size()).arg(t.datable());
        suggestions = seldon::suggest::html(tr("📎 <b>Tableau de datations lu</b> (%1 ligne(s), %2 datable(s)). Que voulez-vous en faire ?")
                                                .arg(t.cells.size()).arg(t.datable()),
                                            seldon::suggest::forTable(t));
    };
    if (seldon::spreadsheet::isSpreadsheet(f)) {
        QString err;
        const QStringList sheets = seldon::spreadsheet::sheetNames(f, &err);
        if (sheets.isEmpty()) {
            QMessageBox::warning(this, tr("Classeur"), tr("Classeur illisible : %1.").arg(err.isEmpty() ? tr("aucune feuille") : err));
            return;
        }
        // Plusieurs feuilles : l'utilisateur choisit ; celle qui contient un tableau de datations est proposée d'abord.
        QString chosen = sheets.first();
        if (sheets.size() > 1) {
            int best = 0;
            for (int i = 0; i < sheets.size(); ++i) {
                seldon::spreadsheet::Sheet sh;
                if (!seldon::spreadsheet::read(f, sheets[i], sh)) continue;
                if (seldon::datatable::fromGrid(sh.rows, QString(), nullptr, sh.lineNumbers).datable() > 0) { best = i; break; }
            }
            bool ok = false;
            chosen = QInputDialog::getItem(this, tr("Feuille à lire"), tr("Le classeur « %1 » a %2 feuilles. Laquelle lire ?")
                                           .arg(QFileInfo(f).fileName()).arg(sheets.size()), sheets, best, false, &ok);
            if (!ok) return;
        }
        seldon::spreadsheet::Sheet sh;
        if (!seldon::spreadsheet::read(f, chosen, sh, &err)) { QMessageBox::warning(this, tr("Classeur"), err); return; }
        QStringList csvLines;                          // feuille en texte (« ; ») : format d'import de ChronoModel, ou repli
        for (const QStringList& r : sh.rows) csvLines << r.join(QLatin1Char(';'));
        const QString asText = csvLines.join(QLatin1Char('\n'));
        seldon::datatable::Table t = seldon::datatable::fromGrid(sh.rows, QFileInfo(f).fileName(), &err, sh.lineNumbers);
        t.sheet = sh.name;
        t.notes += sh.notes;
        if (!seldon::datatable::isChronoModelImport(asText) && t.isValid() && t.datable() > 0) {
            useTable(t);
        } else {
            text = asText.left(8000);
            if (!seldon::datatable::isChronoModelImport(asText))
                QMessageBox::information(this, tr("Classeur"), tr("La feuille « %1 » n'a pas été lue comme un tableau de datations (%2). "
                                                                  "Elle est jointe en texte pour l'assistant ; aucun calcul n'en est tiré.")
                                                                   .arg(sh.name, err.isEmpty() ? tr("aucune ligne datable") : err));
        }
    }
    if (ext == "html" || ext == "htm") {
        // Export des résultats de ChronoModel (Log_Posterior_Distrib_Stats.html…) : trop long pour le modèle, SeldonH en
        // extrait les valeurs utiles (convergence, σ des dates). Autre page HTML : son texte.
        const QString plain = chronoresults::htmlToText(QString::fromUtf8(data));
        if (chronoresults::looksLike(plain)) {
            attachResults_ = chronoresults::parse(plain);
            attachResultsName_ = QFileInfo(f).fileName();
            text = chronoresults::summarize(attachResults_, QFileInfo(f).fileName());
            chronoResults = true;
            suggestions = seldon::suggest::html(tr("📎 <b>Résultats de ChronoModel lus</b> (%1). Questions possibles :").arg(QFileInfo(f).fileName().toHtmlEscaped()),
                                                seldon::suggest::forLog(attachResults_),
                                                tr("<small>Joindre aussi les autres journaux (Log_Model_Description, Log_MCMC_Adaptation…) "
                                                   "aide à localiser la cause d'une non-convergence.</small>"));
        } else {
            text = plain;
        }
    }
    bool curveRead = false;
    if (ext == "csv" || ext == "tsv" || ext == "txt") {
        // Courbe de ChronoModel (mode courbe) : trace des splines (Curve_X_Splines.csv) ou carte de densité (Curve_X_Map.csv,
        // Curve_X_GP_Map.csv). SeldonH garde les tirages et évalue lui-même chaque spline ; le modèle ne reçoit qu'un résumé.
        const QString plain = QString::fromUtf8(data);
        QString err;
        // Courbe de référence du plugin Gauss (« t, valeur, erreur », ex. Curve_X_Gauss_ref.csv exporté par ChronoModel) : gardée
        // pour la conversation, utilisable pour dater une mesure gaussienne (« date une gaussienne x = 45 ± 1 avec la courbe … »).
        if (CurveStore::looksLikeGaussRef(QFileInfo(f).fileName(), plain)) {
            curveRead = true;
            if (CurveStore::addSession(QStringLiteral("gauss"), QFileInfo(f).fileName(), data, &err)) {
                const QString n = QFileInfo(f).completeBaseName().toLower();
                const RefCurve* c = CurveStore::get(QStringLiteral("gauss"), n, CurveFormat::Simple);
                text = QStringLiteral("[Courbe de référence du plugin Gauss « %1 » lue par SeldonH (%2 points, %3 → %4) : pour dater une mesure "
                                      "gaussienne sur cette courbe, méthode gauss avec \"curve\":\"%1\" (outil densite_gauss). N'écris pas "
                                      "les valeurs de la courbe.]").arg(n).arg(c ? c->t.size() : 0)
                           .arg(c ? c->tmin() : 0.0).arg(c ? c->tmax() : 0.0);
                attachTraces_ = dataRead = true;
                suggestions = seldon::suggest::html(tr("📎 <b>Courbe de référence Gauss lue</b> (%1). Questions possibles :").arg(n.toHtmlEscaped()),
                                                    { tr("Date une gaussienne à x = 45 ± 1 avec la courbe %1").arg(n) }, QString());
            } else {
                text = tr("[Courbe de référence illisible : %1]").arg(err);
            }
        } else
        if (seldon::curves::looksLikeSplines(plain)) {
            curveRead = true;
            seldon::SplineTrace st;
            if (seldon::curves::parseSplines(plain, QFileInfo(f).fileName(), st, &err)) {
                seldon::curves::add(st);
                text = seldon::curves::summary(st) + (err.isEmpty() ? QString() : QStringLiteral(" [Lecture : %1.]").arg(err));
                attachTraces_ = dataRead = true;            // lue par SeldonH : le mode calculatrice reste possible
                suggestions = seldon::suggest::html(tr("📎 <b>Trace des splines de ChronoModel lue</b> (%1 : %2 tirages, %3 nœuds, période "
                                                       "%4 → %5). SeldonH évalue chaque courbe tirée comme ChronoModel. Questions possibles :")
                                                        .arg(st.file.toHtmlEscaped()).arg(st.rows).arg(st.knots)
                                                        .arg(seldon::dateformat::format(st.tmin, seldon::dateformat::fromId(settings_.dateFormat)),
                                                             seldon::dateformat::format(st.tmax(), seldon::dateformat::fromId(settings_.dateFormat))),
                                                    seldon::suggest::forCurve(st),
                                                    tr("<small>Joindre aussi <code>Chain_all_Events.csv</code> du même calcul donne la "
                                                       "convergence (chaînes) et permet de citer les Events par leur nom.</small>"));
            } else {
                text = tr("[Trace des splines de ChronoModel illisible : %1]").arg(err);
            }
        } else if (seldon::curves::looksLikeMap(plain)) {
            curveRead = true;
            seldon::CurveMap cm;
            if (seldon::curves::parseMap(plain, QFileInfo(f).fileName(), cm, &err)) {
                seldon::curves::add(cm);
                text = seldon::curves::summary(cm);
                attachTraces_ = dataRead = true;
                suggestions = cm.usable
                    ? seldon::suggest::html(tr("📎 <b>Carte de densité de ChronoModel lue</b> (%1). Elle donne la loi de la courbe à une date "
                                               "donnée ; pour les vitesses entre deux dates et les dates d'atteinte, joindre "
                                               "<code>Curve_X_Splines.csv</code>. Questions possibles :").arg(cm.file.toHtmlEscaped()),
                                            seldon::suggest::forCurveMap(cm))
                    : tr("<p>📎 <b>Carte de densité de ChronoModel lue</b> (%1), mais inutilisable : %2.</p>")
                          .arg(cm.file.toHtmlEscaped(), cm.notes.join(QStringLiteral(" ; ")).toHtmlEscaped());
            } else {
                text = tr("[Carte de densité de ChronoModel illisible : %1]").arg(err);
            }
        }
    }
    if (!curveRead && (ext == "csv" || ext == "tsv" || ext == "txt")) {
        // Export des chaînes MCMC de ChronoModel (Chain_all_Events.csv…) : SeldonH garde les tirages pour les calculs a
        // posteriori (durées, dates entre deux Events) ; le modèle ne reçoit que la liste des colonnes.
        const QString plain = QString::fromUtf8(data);
        if (!seldon::traces::looksLike(plain) && !seldon::datatable::isChronoModelImport(plain)) {
            seldon::datatable::Table t = seldon::datatable::parseText(plain, QFileInfo(f).fileName());
            if (t.isValid() && t.datable() > 0) useTable(t);
        }
        if (seldon::traces::looksLike(plain)) {
            seldon::TraceSet ts;
            QString err;
            if (seldon::traces::parse(plain, QFileInfo(f).fileName(), ts, &err)) {
                seldon::traces::add(ts);
                text = seldon::traces::summary(ts);
                attachTraces_ = dataRead = true;            // lues par SeldonH : le mode calculatrice reste possible
                suggestions = seldon::suggest::html(tr("📎 <b>Traces MCMC lues</b> (%1 : %2 colonne(s), %3 tirages). SeldonH calcule tirage par "
                                                       "tirage, avec la convergence de chaque résultat. Questions possibles :")
                                                        .arg(ts.file.toHtmlEscaped()).arg(ts.columns.size()).arg(ts.rows()),
                                                    seldon::suggest::forTraces(ts),
                                                    tr("<small>Les traces restent chargées pendant la conversation ; vous pouvez joindre aussi "
                                                       "l'autre export (Events ou phases) du même calcul.</small>"));
            } else {
                text = tr("[Fichier de traces MCMC illisible : %1]").arg(err);
            }
        }
    }
    if (ext == "chr" || ext == "json") {
        const QJsonDocument d = QJsonDocument::fromJson(data);
        if (d.isObject() && d.object().contains("events")) {
            text = ChrBuilder::summarize(d.object());
            attachChr_ = d.object();               // dessiné à la demande (« dessine le modèle »)
            attachChrName_ = QFileInfo(f).fileName();
            suggestions = seldon::suggest::html(tr("📎 <b>Projet ChronoModel lu</b>. Questions possibles :"), seldon::suggest::forChr(d.object()));
        }
    }
    if (text.isEmpty()) {
        text = QString::fromUtf8(data);
        const int kMax = 8000;
        if (text.size() > kMax) text = text.left(kMax) + tr("\n[… fichier tronqué à %1 caractères]").arg(kMax);
    }
    const QString name = QFileInfo(f).fileName();
    bool replaced = false;
    for (Attachment& a : attachments_)
        if (a.name == name) { a.text = text; a.data = dataRead; replaced = true; }
    if (!replaced) attachments_.append({ name, text, dataRead });
    attachChronoResults_ = attachChronoResults_ || chronoResults;
    updateAttachLabel();
    // Ne pas laisser l'utilisateur seul devant la zone de question : questions cliquables construites avec ce fichier.
    if (!suggestions.isEmpty()) chat_->addNotice(suggestions);
}

void MainWindow::clearAttachment()
{
    attachments_.clear();
    attachName_.clear();
    attachChrName_.clear();
    attachResultsName_.clear();
    attachChronoResults_ = false;
    attachTable_ = false;
    attachTraces_ = false;
    attachTableHtml_.clear();
    attachResults_ = chronoresults::Report();
    attachChr_ = QJsonObject();
    attachLabel_->hide();
    attachClear_->hide();
}

void MainWindow::onLink(const QUrl& url)
{
    if (url.scheme() != "seldon") { QDesktopServices::openUrl(url); return; }
    const QString action = url.host();
    const QString urlPath = url.path();
    const int idx = QStringView(urlPath).mid(1).toInt();
    if (action == "choose-model") chooseModel();
    else if (action == "open-models") openModelsFolder();
    else if (action == "help") showHelp();
    else if (action == "insert") insertExample(QUrlQuery(url).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded));
    else if (action == "chr") saveChrFromTurn(idx);
    else if (action == "continue") continueAnswer(idx);
    else if (action == "relaunch") relaunchTurn(idx);
    else if (action == "comment") requestComment(idx);
    else if (action == "edit") editLastQuestion();
    else if (action == "thinking") chat_->toggleThinking(idx);
    else if (action == "prompt") exportPrompt(idx);
    else if (action == "figure") showFigure(idx);
    else if (action == "densityfile") saveDensityFile(url);
    else if (action == "tablecsv") saveTableCsv(url);
    else if (action == "tablechr") {
        const QByteArray raw = QByteArray::fromBase64(QUrlQuery(url).queryItemValue(QStringLiteral("spec")).toLatin1(),
                                                      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        const QJsonDocument d = QJsonDocument::fromJson(raw);
        if (d.isObject()) saveChrSpec(d.object());
    }
    else if (action == "copy" && idx >= 0 && idx < turns_.size()) QApplication::clipboard()->setText(shownAnswer(turns_[idx].answer));
    else if (action == "copycard") {
        if (chat_->copyCard(idx)) statusBar()->showMessage(tr("Fiche copiée (texte mis en forme, tableaux et graphiques) : collez-la dans votre traitement de texte."), 5000);
    }
}

void MainWindow::saveChrFromTurn(int turn)
{
    if (turn < 0 || turn >= turns_.size()) return;
    QJsonObject spec;
    QString err;
    if (!ChrBuilder::extractSpec(turns_[turn].answer, spec, &err)) {
        QMessageBox::warning(this, tr("Modèle ChronoModel"), err);
        return;
    }
    saveChrSpec(spec);
}

bool MainWindow::saveChrSpec(const QJsonObject& spec)
{
    const ChrResult r = ChrBuilder::build(spec);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Modèle ChronoModel"),
                             tr("Le modèle proposé est incomplet :\n\n• %1\n\nDemandez à l'assistant de le corriger.").arg(r.errors.join("\n• ")));
        return false;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Enregistrer le modèle ChronoModel"),
        QDir::homePath() + "/" + r.suggestedName + ".chr", tr("Projet ChronoModel (*.chr)"));
    if (path.isEmpty()) return false;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { QMessageBox::warning(this, tr("Enregistrement"), f.errorString()); return false; }
    f.write(QJsonDocument(r.chr).toJson(QJsonDocument::Indented));
    f.close();

    QString html = tr("Modèle enregistré : <code>%1</code> (%2 événements).").arg(path.toHtmlEscaped()).arg(r.chr["events"].toArray().size());
    if (!r.warnings.isEmpty()) {
        html += "<ul>";
        for (const QString& w : r.warnings) html += "<li>" + w.toHtmlEscaped() + "</li>";
        html += "</ul>";
    }
    html += tr("<br><i>Fichier généré automatiquement : vérifiez-le à l'ouverture dans ChronoModel.</i>");
    lastChr_ = r.chr;
    lastChrTitle_ = QFileInfo(path).fileName();
    showChrScenes(r.chr, lastChrTitle_, false);
    logNotice(html);
    return true;
}

void MainWindow::applyRunExtras(const RunOutcome& out)
{
    turnRuns_ += out.requests;
    if (out.chrSpec.isEmpty()) return;
    const ChrResult r = ChrBuilder::build(out.chrSpec);
    if (!r.ok) {
        logNotice(tr("<p><i>⚠ Modèle préparé à partir du tableau, mais incomplet : %1</i></p>").arg(r.errors.join(" ; ").toHtmlEscaped()));
        return;
    }
    lastChr_ = r.chr;                              // « dessine le modèle », « enregistre-le » ensuite
    lastChrTitle_ = r.suggestedName;
    showChrScenes(r.chr, r.suggestedName, true);
}

void MainWindow::saveTableCsv(const QUrl& url)
{
    const QByteArray csv = QByteArray::fromBase64(QUrlQuery(url).queryItemValue(QStringLiteral("data")).toLatin1(),
                                                  QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    if (csv.isEmpty()) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Enregistrer les lignes extraites"),
                                                      QDir::homePath() + QStringLiteral("/extraction.csv"), tr("Tableau CSV (*.csv)"));
    if (path.isEmpty()) return;
    QFile f(path);
    // BOM UTF-8 : Excel lit alors correctement les accents ; séparateur « ; » (Excel et LibreOffice en français).
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write("\xEF\xBB\xBF") < 0 || f.write(csv) < 0) {
        QMessageBox::warning(this, tr("Enregistrement"), f.errorString());
        return;
    }
    logNotice(tr("Lignes enregistrées : <code>%1</code>.").arg(QDir::toNativeSeparators(path).toHtmlEscaped()));
}

void MainWindow::applyPlotBlocks(const QString& answer)
{
    static const QRegularExpression re("^```plot[ \\t]*\\n(.*?)^```",
                                       QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
    auto it = re.globalMatch(answer);
    while (it.hasNext()) {
        const QJsonDocument d = QJsonDocument::fromJson(it.next().captured(1).toUtf8());
        QString err;
        // Loi donnée par ses paramètres (« law ») : points calculés par SeldonH.
        const QJsonObject plot = d.isObject() ? MethodRunner::expandPlotLaws(d.object(), &err) : QJsonObject();
        const bool ok = !plot.isEmpty() && logPlot(plot, &err);
        if (!ok)
            logNotice(tr("<i>📈 Le tracé proposé par l'assistant n'a pas pu être dessiné (%1). Les datations calculées par SeldonH "
                                "sont tracées automatiquement ; un bloc de tracé doit contenir des valeurs x et y explicites.</i>")
                                 .arg((d.isObject() ? err : tr("JSON invalide")).toHtmlEscaped()));
    }
}

// ------------------------------------------------------------------ historique

void MainWindow::logUser(const QString& shown)
{
    chat_->addUser(shown);
    DisplayItem d; d.kind = DisplayItem::User; d.text = shown;
    display_.append(d);
}

void MainWindow::logNotice(const QString& html)
{
    chat_->addNotice(html);
    DisplayItem d; d.kind = DisplayItem::Notice; d.text = html;
    display_.append(d);
}

int MainWindow::recallFigure(const QJsonObject& plot)
{
    const int id = chatFigures_.size();
    chatFigures_.append(ChatFigure{ plot, figures_->lastCurves() });
    const QString title = plot.value("title").toString();
    chat_->addFigure(id, FigurePanel::thumbnailJson(plot, 520, devicePixelRatioF()), title.isEmpty() ? tr("Courbe") : title);
    return id;
}

void MainWindow::showFiguresPanel()
{
    if (figAction_ && !figAction_->isChecked()) figAction_->setChecked(true);     // masqué par le menu Affichage (F4)
    QList<int> sizes = splitter_->sizes();
    if (sizes.size() == 2 && sizes[1] < 80) {                                      // replié en tirant le séparateur
        const int total = sizes[0] + sizes[1];
        splitter_->setSizes({ total * 3 / 5, total * 2 / 5 });
    }
}

void MainWindow::showFigure(int id)
{
    if (id < 0 || id >= chatFigures_.size()) return;
    showFiguresPanel();
    ChatFigure& f = chatFigures_[id];
    if (f.plot.contains("chr_scene")) {                // scène d'un modèle : onglet de scène réutilisé
        showChrScene(f.plot.value("chr").toObject(), f.plot.value("chr_scene").toString(), f.plot.value("title").toString());
        return;
    }
    if (f.page) {
        const QString title = f.plot.value("title").toString();
        figures_->showPage(f.page, title.isEmpty() ? tr("Courbe") : title);
    } else if (figures_->addCurvesJson(f.plot)) {   // onglet fermé entre-temps : recréé
        f.page = figures_->lastCurves();
    }
}

bool MainWindow::logPlot(const QJsonObject& plot, QString* err)
{
    if (!figures_->addCurvesJson(plot, err)) return false;
    recallFigure(plot);
    DisplayItem d; d.kind = DisplayItem::Plot; d.plot = plot;
    display_.append(d);
    return true;
}

int MainWindow::currentQuestionTurn() const
{
    for (int i = turns_.size() - 1; i >= 0; --i)
        if (!turns_[i].synthetic) return i;
    return -1;
}

void MainWindow::logDatingPlot(const QJsonObject& plot, int questionTurn)
{
    if (questionTurn >= 0 && questionTurn == datingPlotTurn_ && datingPlotPage_
        && datingPlotItem_ >= 0 && datingPlotItem_ < display_.size() && display_[datingPlotItem_].kind == DisplayItem::Plot) {
        const QJsonObject merged = tools::mergePlots({ display_[datingPlotItem_].plot, plot });
        if (figures_->replaceCurvesJson(datingPlotPage_, merged)) {
            display_[datingPlotItem_].plot = merged;
            if (datingPlotFig_ >= 0 && datingPlotFig_ < chatFigures_.size()) {
                chatFigures_[datingPlotFig_].plot = merged;
                chat_->setFigure(datingPlotFig_, FigurePanel::thumbnailJson(merged, 520, devicePixelRatioF()));
            }
            return;
        }
    }
    if (!logPlot(plot)) return;
    datingPlotTurn_ = questionTurn;
    datingPlotItem_ = display_.size() - 1;
    datingPlotPage_ = figures_->lastCurves();
    datingPlotFig_ = chatFigures_.size() - 1;
}

int MainWindow::newAssistantBubble()
{
    DisplayItem d; d.kind = DisplayItem::Assistant; d.turn = turns_.size();     // le tour est ajouté juste après
    d.model = modelLabel();
    display_.append(d);
    const int view = chat_->addAssistant();
    chat_->setModelLabel(view, d.model);
    return view;
}

QString MainWindow::calcRequestText() const
{
    return tr("*Calcul demandé par le modèle d'IA : voir la fiche « Calcul SeldonH » ci-dessous.*");
}

QString MainWindow::modelLabel() const
{
    if (!modelReady_ || modelDesc_.isEmpty()) return QString();
    return ChatView::shortModelName(modelDesc_) + (remoteActive() ? tr(" (distant)") : tr(" (local)"));
}

void MainWindow::editLastQuestion()
{
    if (generating_) {
        statusBar()->showMessage(tr("Une réponse est en cours : arrêtez-la avant de modifier la question."), 5000);
        return;
    }
    int u = -1;                                   // dernière question dans l'affichage enregistré
    for (int i = int(display_.size()) - 1; i >= 0 && u < 0; --i)
        if (display_[i].kind == DisplayItem::User) u = i;
    const int view = chat_->lastUserIndex();
    if (u < 0 || view < 0) return;
    // Tours de cette question : ceux des réponses affichées après elle (et les tours de suite qui les suivent).
    int firstTurn = -1;
    for (int i = u; i < display_.size(); ++i)
        if (display_[i].kind == DisplayItem::Assistant && display_[i].turn >= 0)
            firstTurn = firstTurn < 0 ? display_[i].turn : std::min(firstTurn, display_[i].turn);
    QString text = firstTurn >= 0 && firstTurn < turns_.size() && !turns_[firstTurn].synthetic ? turns_[firstTurn].question
                                                                                              : chat_->userText(view);
    // Mentions ajoutées à l'affichage (pièce jointe, question relancée) : pas dans le texte à corriger.
    bool hadAttachment = false;
    for (const QString& tag : { QStringLiteral("\n\n📎 "), QStringLiteral("\n\n↻ ") }) {
        const qsizetype k = text.indexOf(tag);
        if (k >= 0) { hadAttachment = hadAttachment || tag.contains(QStringLiteral("📎")); text.truncate(k); }
    }
    display_.resize(u);
    chat_->truncate(view);
    if (firstTurn >= 0 && firstTurn < turns_.size()) turns_.resize(firstTurn);
    if (datingPlotItem_ >= u) { datingPlotTurn_ = datingPlotItem_ = datingPlotFig_ = -1; datingPlotPage_ = nullptr; }
    if (!turns_.isEmpty()) { computeSuites(); setTurnActions(int(turns_.size()) - 1); }
    input_->setPlainText(text.trimmed());
    input_->setFocus();
    QTextCursor c = input_->textCursor();
    c.movePosition(QTextCursor::End);
    input_->setTextCursor(c);
    updateConvTitle();
    saveCurrent();
    statusBar()->showMessage(hadAttachment
        ? tr("Question remise dans la zone de saisie. Joignez de nouveau le fichier si besoin, puis envoyez.")
        : tr("Question remise dans la zone de saisie : corrigez-la puis envoyez."), 7000);
}

void MainWindow::logAssistantText(int turn, const QString& markdown, const QString& thinking)
{
    for (int i = display_.size() - 1; i >= 0; --i) {
        if (display_[i].kind == DisplayItem::Assistant && display_[i].turn == turn) {
            display_[i].text = markdown;
            display_[i].thinking = thinking;
            return;
        }
    }
}

void MainWindow::setTurnActions(int turn)
{
    if (turn < 0 || turn >= turns_.size() || turns_[turn].view < 0) return;
    const Turn& t = turns_[turn];
    QStringList actions;
    QJsonObject spec;
    if (ChrBuilder::extractSpec(t.answer, spec) && ChrBuilder::build(spec).ok)      // seulement un modèle complet et valide
        actions << QStringLiteral("<a href=\"seldon://chr/%1\">💾 Enregistrer le modèle (.chr)</a>").arg(turn);
    if (t.truncation && turn == turns_.size() - 1)
        actions << tr("<a href=\"seldon://continue/%1\">▶ Continuer la réponse</a>").arg(turn);
    if (t.calcOnly)
        actions << tr("<a href=\"seldon://comment/%1\" title=\"Le modèle commente le résultat affiché (sans le recopier)\">💬 Demander un commentaire</a>").arg(turn);
    if (!t.answer.isEmpty())
        actions << QStringLiteral("<a href=\"seldon://copy/%1\">Copier</a>").arg(turn);
    if (!t.answer.isEmpty() || t.truncation)
        actions << tr("<a href=\"seldon://relaunch/%1\" title=\"Redemander la même question avec le même prompt\">↻ Relancer</a>").arg(turn);
    if (!t.prompt.isEmpty())
        actions << tr("<a href=\"seldon://prompt/%1\" title=\"Enregistrer exactement ce qui a été envoyé au modèle (JSON ou Markdown)\">📄 Exporter le prompt</a>").arg(turn);
    if (!actions.isEmpty()) chat_->setActions(t.view, actions);
    // Suites proposées : seulement sous la dernière réponse ; un clic recopie la phrase dans la zone de question.
    QString suites;
    if (turn == turns_.size() - 1 && !t.suites.isEmpty()) {
        QStringList links;
        for (const QString& q : t.suites)
            links << QStringLiteral("<a href=\"seldon://insert?q=%1\">%2</a>").arg(QString::fromLatin1(QUrl::toPercentEncoding(q)), q.toHtmlEscaped());
        suites = tr("↪ Suites : %1").arg(links.join(QStringLiteral(" · ")));
    }
    chat_->setSuggestions(t.view, suites);
}

void MainWindow::computeSuites()
{
    if (turns_.isEmpty()) return;
    Turn& t = turns_.last();
    seldon::suggest::After a;
    a.requests = turnRuns_;
    QJsonObject spec;
    a.modelBuilt = (ChrBuilder::extractSpec(t.answer, spec) && ChrBuilder::build(spec).ok)
                   || std::any_of(turnRuns_.begin(), turnRuns_.end(), [](const QJsonObject& r) {
                          return r.value("method").toString() == QLatin1String("table") && r.value("action").toString() == QLatin1String("model"); });
    for (const seldon::TraceSet& ts : seldon::traces::loaded()) a.traceColumns << ts.columns;
    if (turnHasResults_) a.log = &lastResults_;
    t.suites = seldon::suggest::afterResult(a);
}

void MainWindow::saveCurrent()
{
    if (!settings_.keepHistory || convId_.isEmpty() || turns_.isEmpty()) return;
    Conversation c;
    c.id = convId_;
    c.created = convCreated_.isValid() ? convCreated_ : QDateTime::currentDateTime();
    c.updated = QDateTime::currentDateTime();
    c.backend = remoteActive() ? tr("modèle distant : %1").arg(settings_.remoteModel)
                               : tr("modèle local : %1").arg(modelDesc_);
    for (const Turn& t : std::as_const(turns_)) {
        StoredTurn s;
        s.question = t.question; s.modelUser = t.modelUser; s.answer = t.answer; s.toolBlocks = t.toolBlocks; s.prompt = t.prompt;
        s.skillIds = t.skillIds; s.followUp = t.followUp; s.synthetic = t.synthetic; s.calcOnly = t.calcOnly; s.suites = t.suites;
        c.turns.append(s);
    }
    c.display = display_;
    if (!c.hasContent()) return;
    for (const StoredTurn& s : std::as_const(c.turns))
        if (!s.synthetic) { c.title = Conversation::makeTitle(s.question); break; }
    QString err;
    if (!store_.save(c, &err)) {
        statusBar()->showMessage(tr("Historique : conversation non enregistrée (%1)").arg(err), 8000);
        return;
    }
    touchHistoryEntry(c);
}

void MainWindow::touchHistoryEntry(const Conversation& c)
{
    ConversationInfo i;
    i.id = c.id; i.title = c.title; i.updated = c.updated; i.exchanges = c.exchanges(); i.backend = c.backend;
    bool found = false;
    for (ConversationInfo& e : historyItems_)
        if (e.id == i.id) { e = i; found = true; break; }
    if (!found) historyItems_.append(i);
    std::sort(historyItems_.begin(), historyItems_.end(), [](const ConversationInfo& a, const ConversationInfo& b) {
        return a.updated != b.updated ? a.updated > b.updated : a.id > b.id;
    });
    history_->setItems(historyItems_, convId_);
}

void MainWindow::refreshHistory()
{
    historyItems_ = store_.list();
    history_->setItems(historyItems_, convId_);
}

void MainWindow::openConversation(const QString& id)
{
    if (generating_) {
        statusBar()->showMessage(tr("Une réponse est en cours : attendez sa fin (ou arrêtez-la) avant d'ouvrir une autre conversation."), 6000);
        return;
    }
    if (id == convId_) return;                    // déjà affichée
    Conversation c;
    QString err;
    if (!store_.load(id, c, &err)) {
        QMessageBox::warning(this, tr("Historique"), tr("Cette conversation n'a pas pu être ouverte :\n%1").arg(err));
        refreshHistory();
        return;
    }
    saveCurrent();                                // on ne perd pas la conversation qu'on quitte
    resetConversation(true);

    convId_ = c.id;
    convCreated_ = c.created;
    for (const StoredTurn& s : std::as_const(c.turns)) {
        Turn t;
        t.question = s.question; t.modelUser = s.modelUser; t.answer = s.answer; t.toolBlocks = s.toolBlocks; t.prompt = s.prompt;
        t.skillIds = s.skillIds; t.followUp = s.followUp; t.synthetic = s.synthetic; t.calcOnly = s.calcOnly; t.suites = s.suites;
        turns_.append(t);
    }
    for (const DisplayItem& d : std::as_const(c.display)) {
        switch (d.kind) {
        case DisplayItem::User:
            chat_->addUser(d.text);
            break;
        case DisplayItem::Notice:
            chat_->addNotice(d.text);
            break;
        case DisplayItem::Plot:
            if (d.plot.contains("chr_scene")) addSceneFigure(d.plot);      // vignette ; la scène s'ouvre au clic
            else if (figures_->addCurvesJson(d.plot)) recallFigure(d.plot);
            break;
        case DisplayItem::Assistant: {
            const int view = chat_->addAssistant();
            turns_[d.turn].view = view;
            chat_->setAssistant(view, d.text, d.thinking, false, true);
            chat_->setModelLabel(view, d.model);
            if (turns_[d.turn].calcOnly || d.text == calcRequestText()) chat_->setAppNote(view, true);
            setTurnActions(d.turn);
            break;
        }
        }
        display_.append(d);
    }
    for (int i = int(turns_.size()) - 1; i >= 0 && lastChr_.isEmpty(); --i) {     // « dessine le modèle » possible ensuite
        QJsonObject spec;
        if (!ChrBuilder::extractSpec(turns_[i].answer, spec)) continue;
        const ChrResult r = ChrBuilder::build(spec);
        if (r.ok) { lastChr_ = r.chr; lastChrTitle_ = r.suggestedName; }
    }
    history_->setItems(historyItems_, convId_);
    updateConvTitle();
    statusBar()->showMessage(tr("Conversation rouverte : %1. Vous pouvez la poursuivre.").arg(c.title), 6000);
}

void MainWindow::deleteConversation(const QString& id)
{
    if (generating_ && id == convId_) {
        statusBar()->showMessage(tr("Une réponse est en cours dans cette conversation : arrêtez-la avant de la supprimer."), 6000);
        return;
    }
    QString title = id;
    for (const ConversationInfo& i : std::as_const(historyItems_)) if (i.id == id) title = i.title;
    const auto ans = QMessageBox::question(this, tr("Supprimer la conversation"),
                                           tr("Supprimer définitivement la conversation « %1 » ?").arg(title));
    if (ans != QMessageBox::Yes) return;
    store_.remove(id);
    historyItems_.erase(std::remove_if(historyItems_.begin(), historyItems_.end(),
                                       [&](const ConversationInfo& i) { return i.id == id; }), historyItems_.end());
    if (id == convId_) {                          // la conversation affichée disparaît aussi : on repart d'un écran vide
        resetConversation(false);
        showWelcome();
    }
    history_->setItems(historyItems_, convId_);
    statusBar()->showMessage(tr("Conversation supprimée."), 4000);
}

void MainWindow::setHistoryVisible(bool on)
{
    history_->setVisible(on);
    historyStrip_->setChecked(on);
    if (historyAction_ && historyAction_->isChecked() != on) {
        QSignalBlocker block(historyAction_);
        historyAction_->setChecked(on);
    }
    QSettings().setValue("ui/historyVisible", on);
}

void MainWindow::deleteAllConversations()
{
    if (generating_) {
        statusBar()->showMessage(tr("Une réponse est en cours : attendez sa fin avant de tout effacer."), 6000);
        return;
    }
    const auto ans = QMessageBox::question(this, tr("Tout effacer"),
                                           tr("Supprimer définitivement les %1 conversation(s) enregistrée(s), y compris celle qui est affichée ?")
                                               .arg(std::max<qsizetype>(historyItems_.size(), 1)));
    if (ans != QMessageBox::Yes) return;
    const int n = store_.removeAll();
    historyItems_.clear();
    resetConversation(false);
    showWelcome();
    history_->setItems(historyItems_, convId_);
    statusBar()->showMessage(tr("%1 conversation(s) supprimée(s).").arg(n), 5000);
}

// --------------------------------------------------------------------- divers

// Export de la conversation : page HTML (un seul fichier, images intégrées) ou document Word (.docx), figures comprises.
void MainWindow::saveTranscript()
{
    const QString htmlFilter = tr("Page HTML (*.html)"), docxFilter = tr("Document Word (*.docx)");
    QString filter = htmlFilter;
    QString path = QFileDialog::getSaveFileName(this, tr("Exporter la conversation"),
                                                QDir::homePath() + "/conversation_seldonh.html", htmlFilter + ";;" + docxFilter, &filter);
    if (path.isEmpty()) return;
    // Format : extension écrite par l'utilisateur, sinon le filtre choisi.
    const QString suffix = QFileInfo(path).suffix().toLower();
    bool docx = suffix == QLatin1String("docx") || (suffix != QLatin1String("html") && suffix != QLatin1String("htm") && filter == docxFilter);
    if (docx && suffix != QLatin1String("docx")) path += QStringLiteral(".docx");
    if (!docx && suffix != QLatin1String("html") && suffix != QLatin1String("htm")) path += QStringLiteral(".html");
    QString err;
    bool ok = false;
    if (docx) {
        const QString title = tr("Conversation SeldonH du %1").arg(QLocale().toString(convCreated_.isValid() ? convCreated_ : QDateTime::currentDateTime(),
                                                                                     QLocale::ShortFormat));
        ok = chat_->exportDocx(path, title, &err);
    } else {
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly) && f.write(chat_->exportHtml().toUtf8()) >= 0;
        if (!ok) err = f.errorString();
    }
    if (ok) statusBar()->showMessage(tr("Conversation exportée : %1").arg(QDir::toNativeSeparators(path)), 6000);
    else QMessageBox::warning(this, tr("Exporter la conversation"), tr("Export impossible : %1").arg(err));
}

void MainWindow::openModelsFolder()
{
    const QString dir = AppSettings::modelsDropDir();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::openSkillsFolder()
{
    QDir().mkpath(settings_.skillsDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(settings_.skillsDir));
}

void MainWindow::openCurvesFolder()
{
    for (const char* m : { "c14", "f14c", "am", "gauss", "density" })
        QDir().mkpath(QDir(settings_.curvesDir).filePath(CurveStore::subdir(QString::fromLatin1(m))));
    QDesktopServices::openUrl(QUrl::fromLocalFile(settings_.curvesDir));
}

void MainWindow::reloadCurves()
{
    CurveStore::setExtraRoots(settings_.curveRoots());
    int n = 0;
    for (const char* m : { "c14", "f14c", "am" }) n += CurveStore::names(QString::fromLatin1(m)).size();
    statusBar()->showMessage(tr("%1 courbes de référence disponibles").arg(n), 4000);
}

// Liste des courbes par méthode, avec leur origine (dossier de l'utilisateur, ChronoModel ou intégrée).
void MainWindow::showCurves()
{
    logNotice(curvesListHtml());
}

// Liste complète des courbes de référence (menu Courbes, et question « liste les courbes de réf ») : 14C, F14C,
// archéomagnétisme, plugin Gauss (fichiers .csv « t, valeur, erreur ») et densités du plugin Density, avec leur origine.
QString MainWindow::curvesListHtml(QString* plain) const
{
    const QString cmDir = AppSettings::chronoModelCalibDir();
    auto origin = [&](const QString& path) {
        if (path.startsWith(QLatin1String(":/"))) return tr("intégrée");
        if (path.startsWith(QStringLiteral("📎"))) return tr("jointe à la conversation");
        if (!cmDir.isEmpty() && path.startsWith(cmDir)) return tr("ChronoModel");
        return tr("utilisateur");
    };
    QString html = tr("<b>Courbes de référence</b><br><i>Ordre de recherche : votre dossier (%1), puis ChronoModel (%2), "
                      "puis les courbes intégrées. Une courbe se désigne par son nom de fichier sans extension.</i>")
                       .arg(settings_.curvesDir.toHtmlEscaped(),
                            settings_.useChronoModelCurves ? cmDir.toHtmlEscaped() : tr("désactivé"));
    const struct { const char* id; const char* title; } methods[] = {
        { "c14", "Radiocarbone (14C, fichiers .14c)" }, { "f14c", "F14C post-bombe (fichiers .14c)" },
        { "am", "Archéomagnétisme (fichiers .ref)" },
        { "gauss", "Plugin Gauss : courbes de référence (fichiers .csv « t, valeur, erreur », dossier Gauss)" },
        { "density", "Plugin Density : densités tabulées (fichiers .csv, dossier Density)" } };
    QString text = QStringLiteral("Courbes de référence disponibles (liste complète) :\n");
    // Une courbe par ligne (liste à puces), avec son origine.
    for (const auto& m : methods) {
        const QStringList names = CurveStore::names(QString::fromLatin1(m.id));
        html += QStringLiteral("<p><b>%1</b> — %2</p><ul>").arg(tr(m.title), tr("%n courbe(s)", nullptr, int(names.size())));
        QStringList line;
        for (const QString& n : names) {
            const QString o = origin(CurveStore::sourceOf(QString::fromLatin1(m.id), n));
            html += QStringLiteral("<li><code>%1</code> <i>(%2)</i></li>").arg(n.toHtmlEscaped(), o);
            line << QStringLiteral("%1 (%2)").arg(n, o);
        }
        if (names.isEmpty()) html += QStringLiteral("<li><i>%1</i></li>").arg(tr("aucune"));
        html += QStringLiteral("</ul>");
        text += QStringLiteral("- %1 : %2\n").arg(tr(m.title), line.isEmpty() ? tr("aucune") : line.join(QStringLiteral(", ")));
    }
    if (plain) *plain = text;
    return html;
}

// Lien « Enregistrer le fichier… » d'une fiche de densité : la spécification est dans le lien, le fichier est recalculé.
// Dossier proposé : Calib/Density de ChronoModel s'il est installé (le fichier y est directement utilisable).
void MainWindow::saveDensityFile(const QUrl& url)
{
    const QByteArray raw = QByteArray::fromBase64(QUrlQuery(url).queryItemValue(QStringLiteral("spec")).toLatin1(),
                                                  QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const DensityFileResult F = buildDensityFile(QJsonDocument::fromJson(raw).object());
    if (!F.ok) { QMessageBox::warning(this, tr("Fichier de densité"), F.error); return; }

    const QString cmCalib = AppSettings::chronoModelCalibDir();
    QString dir = QDir::homePath();
    if (!cmCalib.isEmpty() && QDir(cmCalib).exists()) {
        dir = QDir(cmCalib).filePath(QStringLiteral("Density"));
        QDir().mkpath(dir);
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Enregistrer le fichier de densité"), QDir(dir).filePath(F.fileName),
                                                      tr("Densité ChronoModel (*.csv)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) || f.write(F.csv.toUtf8()) < 0) {
        QMessageBox::warning(this, tr("Fichier de densité"), tr("Impossible d'écrire %1.").arg(path));
        return;
    }
    f.close();
    const bool inCm = !cmCalib.isEmpty() && QFileInfo(path).absolutePath().startsWith(cmCalib);
    logNotice(tr("📄 Fichier de densité enregistré : <code>%1</code>.<br>%2 Dans un modèle : "
                 "<code>{\"type\":\"density\",\"curve\":\"%3\"}</code>.")
                  .arg(path.toHtmlEscaped(),
                       inCm ? tr("Il est dans le dossier des densités de ChronoModel : relancez ChronoModel pour qu'il apparaisse.")
                            : tr("Copiez-le dans le dossier <code>Calib/Density</code> de ChronoModel (%1), puis relancez ChronoModel.")
                                  .arg(cmCalib.isEmpty() ? tr("introuvable sur cet ordinateur") : cmCalib.toHtmlEscaped()),
                       QFileInfo(path).fileName().toHtmlEscaped()));
}

void MainWindow::reloadSkills()
{
    const int n = skills_.reload(settings_.skillsDir);
    lexicon_.reload(settings_.skillsDir);
    statusBar()->showMessage(tr("%1 compétences chargées").arg(n), 4000);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("À propos de SeldonH"),
                       tr("<b>SeldonH</b><br>Assistant local d'aide à la datation et à la modélisation chronologique.<br>"
                          "Moteur : llama.cpp (exécution locale).<br>%1 compétences chargées.").arg(skills_.skills().size()));
}
