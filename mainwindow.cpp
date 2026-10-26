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
#include "src/methods/common/Tools.h"
#include "src/llm/RemoteEngine.h"
#include "src/ui/ChatView.h"
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
    skills_.reload(settings_.skillsDir);
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
    });
    remote_->setTools(settings_.remoteTools ? tools::schemaForApi() : QJsonArray());

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
    connect(chat_, &QTextBrowser::anchorClicked, this, &MainWindow::onLink);

    input_ = new InputEdit;
    connect(input_, &InputEdit::submitted, this, &MainWindow::sendMessage);

    sendBtn_ = new QPushButton(tr("Envoyer"));
    sendBtn_->setDefault(true);
    connect(sendBtn_, &QPushButton::clicked, this, &MainWindow::sendMessage);
    stopBtn_ = new QPushButton(tr("Arrêter"));
    connect(stopBtn_, &QPushButton::clicked, this, &MainWindow::stopGeneration);

    attachBtn_ = new QToolButton;
    attachBtn_->setText(tr("📎 Joindre"));
    attachBtn_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    attachBtn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);   // même largeur que « Envoyer »
    attachBtn_->setToolTip(tr("Joindre un fichier (.chr, .csv, .txt…) ou une image"));
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
    buttons->addStretch();

    auto* inputRow = new QHBoxLayout;
    inputRow->addWidget(input_, 1);
    inputRow->addLayout(buttons);

    auto* left = new QWidget;
    auto* leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(6, 6, 6, 6);
    leftLay->addWidget(chat_, 1);
    leftLay->addLayout(attachRow);
    leftLay->addLayout(inputRow);

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
    connect(history_, &HistoryPanel::newRequested, this, &MainWindow::newConversation);

    modelLabel_ = new QLabel(tr("Aucun modèle"));
    statsLabel_ = new QLabel;
    loadBar_ = new QProgressBar;
    loadBar_->setRange(0, 100);
    loadBar_->setMaximumWidth(160);
    loadBar_->hide();
    statusBar()->addWidget(modelLabel_);
    statusBar()->addWidget(loadBar_);
    statusBar()->addPermanentWidget(statsLabel_);
}

void MainWindow::buildMenus()
{
    QMenu* file = menuBar()->addMenu(tr("&Fichier"));
    file->addAction(tr("Nouvelle conversation"), QKeySequence::New, this, &MainWindow::newConversation);
    file->addAction(tr("Ouvrir un modèle (.gguf)…"), QKeySequence::Open, this, &MainWindow::chooseModel);
    file->addAction(tr("Ouvrir le dossier des modèles"), this, &MainWindow::openModelsFolder);
    file->addAction(tr("Joindre un fichier…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), this, &MainWindow::attachFile);
    file->addAction(tr("Enregistrer la conversation (HTML)…"), QKeySequence::Save, this, &MainWindow::saveTranscript);
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
    chat_->addNotice(
        tr("<b>Bienvenue dans SeldonH</b><br>"
           "%1 pour :"
           "<ul><li>réaliser des <b>datations simples</b> et vérifier leur cohérence ;</li>"
           "<li><b>construire un modèle ChronoModel</b> (événements, phases, contraintes) et l'exporter en <code>.chr</code> ;</li>"
           "<li><b>interpréter les résultats</b> (joignez un <code>.chr</code> ou un export CSV avec 📎).</li></ul>")
              .arg(settings_.useRemote ? tr("Assistant utilisant un <b>modèle distant</b> (vos questions sont envoyées au serveur configuré ; "
                                            "les calculs de datation restent locaux)")
                                       : tr("Assistant local (aucune donnée ne quitte votre ordinateur)"))
        + tr("Pour essayer, cliquez sur un exemple (il est recopié dans la zone de question) :<ul>"
             "<li>%1</li><li>%2</li><li>%3</li></ul>")
              .arg(ex(QStringLiteral("C14 de 3560 ± 20 BP")),
                   ex(QStringLiteral("archéomagnétisme : inclinaison 65°, alpha95 = 1")),
                   ex(QStringLiteral("date uniforme entre -500 et -400")))
        + tr("<a href=\"seldon://help\">Mode d'emploi</a> (F1 ou <code>/aide</code>) — "
             "les compétences de l'assistant sont des fiches Markdown que vous pouvez enrichir (menu Compétences)."));
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
    remote_->setTools(settings_.remoteTools ? tools::schemaForApi() : QJsonArray());
    if (!cfg.isValid()) {
        modelReady_ = false;
        modelLabel_->setText(tr("Modèle distant non configuré"));
        chat_->addNotice(tr("<b>Modèle distant incomplet.</b> Renseignez la Base URL et le nom du modèle dans "
                            "<i>Édition &gt; Préférences</i>."));
    } else {
        modelReady_ = true;
        modelDesc_ = cfg.model;
        modelLabel_->setText(tr("☁ Modèle distant : %1 — %2").arg(cfg.model, remote::hostOf(cfg.baseUrl)));
        chat_->addNotice(tr("☁ <b>Modèle distant actif</b> : <code>%1</code> sur <code>%2</code>. Vos questions, les compétences "
                            "ajoutées et les fichiers joints sont envoyés à ce serveur ; les calculs de datation restent faits "
                            "sur cet ordinateur.").arg(cfg.model.toHtmlEscaped(), remote::hostOf(cfg.baseUrl).toHtmlEscaped()));
    }
    updateControls();
}

void MainWindow::onModelLoaded(const QString& description, int ctx)
{
    loading_ = false;
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
    modelReady_ = false;
    loadBar_->hide();
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
    if (remote_) remote_->setTools(settings_.remoteTools ? tools::schemaForApi() : QJsonArray());
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

QStringList MainWindow::recentSkillIds(int n) const
{
    QStringList ids;
    for (int i = std::max(0, int(turns_.size()) - n); i < turns_.size(); ++i) ids << turns_[i].skillIds;
    return ids;
}

QVector<ChatMessage> MainWindow::buildChat() const
{
    QVector<ChatMessage> v;
    const std::string userPrompt = settings_.userPrompt.toStdString();
    v.append({ "system", skills_.alwaysText().toStdString() + dateformat::systemHint(dateformat::fromId(settings_.dateFormat))
                 + ((remoteActive() && settings_.remoteTools) ? toolsSystemHint() : std::string()) + userPromptSystemBlock(userPrompt) });
    for (int i = 0; i < turns_.size(); ++i) {
        const Turn& t = turns_[i];
        const bool last = (i == turns_.size() - 1);
        const QString said = (t.answer.trimmed() + t.toolBlocks).trimmed();     // réponse + appels d'outils réécrits en blocs
        if (!last && said.isEmpty()) continue;       // tour interrompu/en erreur : ignoré
        v.append({ "user", t.modelUser.toStdString() + (last ? userPromptReminder(userPrompt) : std::string()) });
        if (!last) v.append({ "assistant", said.toStdString() });
    }
    return v;
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

    if (generating_) return;
    if (!modelReady_) {
        statusBar()->showMessage(loading_ ? tr("Chargement du modèle en cours…")
                                 : remoteActive() ? tr("Modèle distant non configuré : Édition > Préférences. La commande /aide fonctionne sans modèle.")
                                                  : tr("Aucun modèle chargé : Fichier > Ouvrir un modèle (.gguf). La commande /aide fonctionne sans modèle."), 6000);
        return;
    }
    const QString q = input_->toPlainText().trimmed();
    if (q.isEmpty()) return;

    if (convId_.isEmpty()) {                      // première question : la conversation reçoit son identifiant
        convId_ = Conversation::newId();
        convCreated_ = QDateTime::currentDateTime();
    }

    // Demande de création de modèle : la fiche qui décrit le format du bloc chronomodel est toujours fournie.
    QStringList pinned = MethodRunner::isModelRequest(q) ? QStringList{ QStringLiteral("03_chronomodel_creer_modele") } : QStringList();
    // Résultats de ChronoModel joints : fiche de diagnostic de l'utilisateur si elle existe, sinon la fiche intégrée des logs.
    if (attachChronoResults_) {
        bool user = false;
        for (const Skill& s : skills_.skills()) user = user || s.id == QLatin1String("chronomodel_resultats_convergence");
        pinned << (user ? QStringLiteral("chronomodel_resultats_convergence") : QStringLiteral("11_chronomodel_logs"));
    }
    const QList<SkillMatch> matches = skills_.select(q, settings_.skillChars, recentSkillIds(3), pinned);
    const QString knowledge = SkillManager::format(matches);

    if (!turns_.isEmpty() && turns_.last().truncation) {     // nouvelle question : plus de « Continuer » sur la précédente
        turns_.last().truncation = 0;
        setTurnActions(turns_.size() - 1);
    }

    Turn t;
    t.question = q;
    for (const SkillMatch& m : matches) t.skillIds << m.skill->id;

    // Calculs déterministes (calibration 14C) : faits par l'application, pas par le modèle.
    RunOutcome dating = MethodRunner::analyzeQuestion(q, dateformat::fromId(settings_.dateFormat));
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
    t.followUp = dating.found;       // déjà calculé par l'application : pas de nouveau calcul à relancer

    QString mu;
    if (dating.found)          mu += dating.promptBlock + "\n";
    if (!knowledge.isEmpty())  mu += "[Connaissances de référence]\n" + knowledge + "\n\n";
    if (!attachText_.isEmpty()) mu += "[Fichier joint : " + attachName_ + "]\n" + attachText_ + "\n\n";
    t.modelUser = mu.isEmpty() ? q : mu + "[Question]\n" + q;

    QString shown = q;
    if (!attachName_.isEmpty()) shown += "\n\n📎 " + attachName_;
    logUser(shown);
    if (dating.found) {
        logNotice(dating.noticeHtml);
        if (!dating.plot.isEmpty()) logDatingPlot(dating.plot, turns_.size());     // le tour t est ajouté juste après
        for (const QJsonObject& p : dating.extraPlots) logPlot(p);
    }
    t.view = newAssistantBubble();
    turns_.append(t);

    input_->clear();
    clearAttachment();

    saveCurrent();                                // la question est gardée même si l'application s'arrête pendant la réponse
    startGeneration();
}

void MainWindow::startGeneration()
{
    raw_.clear();
    startsInThink_ = settings_.thinking;      // corrigé par le signal `started`
    generating_ = true;
    updateControls();
    statsLabel_->setText(tr("Évaluation du prompt…"));

    SamplingParams sp;
    sp.temperature    = static_cast<float>(settings_.temperature);
    sp.maxTokens      = settings_.effectiveMaxTokens(remoteActive());   // + réserve pour la réflexion (modèle local)
    sp.enableThinking = settings_.thinking;
    if (remoteActive()) emit requestRemoteGenerate(buildChat(), sp);
    else emit requestGenerate(buildChat(), sp);
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

    QVector<ChatMessage> chat = buildChat();      // se termine par la question de ce tour
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
    if (remoteActive()) emit requestRemoteGenerate(chat, sp);
    else emit requestGenerate(chat, sp);
}

// Le bloc ```chronomodel n'est pas montré tel quel : un résumé (modèle prêt / ce qui manque) le remplace, sauf si la
// dernière vraie question de l'utilisateur demande explicitement le JSON.
QString MainWindow::shownAnswer(const QString& answer) const
{
    bool showJson = false;
    for (int i = turns_.size() - 1; i >= 0; --i)
        if (!turns_[i].synthetic) { showJson = ChrBuilder::asksForJson(turns_[i].question); break; }
    return ChrBuilder::displayMarkdown(answer, showJson);
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
    const QVector<QJsonObject> blocks = MethodRunner::extractCalcBlocks(turns_.last().answer);
    if (blocks.isEmpty()) return false;

    const RunOutcome out = MethodRunner::run(blocks, displayFormat());
    if (!out.found) return false;
    logNotice(out.noticeHtml);
    if (!out.plot.isEmpty()) logDatingPlot(out.plot, currentQuestionTurn());
    for (const QJsonObject& p : out.extraPlots) logPlot(p);

    Turn t;
    t.question = tr("(résultat du calcul demandé par l'assistant)");
    t.modelUser = out.promptBlock;
    t.followUp = true;
    t.synthetic = true;
    t.view = newAssistantBubble();
    turns_.append(t);
    startGeneration();
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
    const QString thinking = continuing_ ? turns_.last().thinking : QString::fromStdString(s.thinking);
    chat_->setAssistant(turns_.last().view, shownAnswer(QString::fromStdString(s.answer)), thinking, !continuing_ && s.thinkingOpen, false);
}

void MainWindow::finishAssistantMessage(bool interrupted)
{
    if (turns_.isEmpty()) return;
    Turn& t = turns_.last();
    const SplitText s = splitThinking(raw_.toStdString(), startsInThink_);
    t.answer = QString::fromStdString(s.answer);       // texte complet (avec le bloc de modèle) : historique, enregistrement
    QString answer = shownAnswer(t.answer);
    if (!continuing_) t.thinking = QString::fromStdString(s.thinking);
    if (interrupted) answer += tr("\n\n*(réponse interrompue)*");
    else if (t.truncation) answer += QStringLiteral("\n\n*%1*").arg(truncationNotice(t));
    chat_->setAssistant(t.view, answer, t.thinking, false, true);
    logAssistantText(turns_.size() - 1, answer, t.thinking);
    setTurnActions(turns_.size() - 1);

    applyPlotBlocks(t.answer);
}

void MainWindow::onFinished(const GenStats& st)
{
    generating_ = false;
    if (!turns_.isEmpty()) {
        Turn& t = turns_.last();
        t.truncation = st.stoppedByUser ? 0 : st.truncation;
        t.tokenLimit = st.tokenLimit;
        t.truncationDetail = QString::fromStdString(st.truncationDetail);
    }
    finishAssistantMessage(st.stoppedByUser);
    continuing_ = false;
    saveCurrent();

    const double tps = st.genMs > 0 ? st.generated * 1000.0 / st.genMs : 0.0;
    if (!remoteActive() && st.generated >= 32) lastLocalTps_ = tps;     // pour « Optimiser pour ce modèle »
    QString msg = tr("%1 tokens générés — %2 tok/s").arg(st.generated).arg(tps, 0, 'f', 1);
    if (st.droppedMessages > 0)
        msg += tr(" — %1 ancien(s) message(s) oublié(s) (contexte plein)").arg(st.droppedMessages);
    if (st.contextFull) msg += tr(" — contexte saturé");
    if (!st.stoppedByUser && st.truncation) msg += tr(" — réponse coupée");

    if (!st.stoppedByUser && runRequestedCalculations()) return;      // relance l'assistant avec le résultat
    updateControls();
    statsLabel_->setText(msg);
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
    for (const ToolCall& c : calls) {
        const tools::ToolOutcome o = tools::execute(c.name, c.arguments, fmt);
        results.append({ c.id, c.name, o.content });
        if (o.executed) {
            logNotice(o.run.noticeHtml);
            plots << o.run.plot;
            extra += o.run.extraPlots;
            blocks += QStringLiteral("\n\n```calcul\n%1\n```").arg(QString::fromUtf8(QJsonDocument(o.request).toJson(QJsonDocument::Compact)));
            forModel << o.content;
        } else {
            forModel << tr("[Retour de l'outil « %1 » (SeldonH)]\n%2").arg(c.name, o.content);
            if (c.name != QLatin1String("courbes_disponibles"))
                logNotice(tr("<i>🧮 L'assistant a appelé l'outil <code>%1</code> mais le calcul n'a pas pu être lancé : %2</i>")
                                     .arg(c.name.toHtmlEscaped(), o.content.toHtmlEscaped()));
        }
    }
    const QJsonObject plot = tools::mergePlots(plots);
    if (!plot.isEmpty()) logDatingPlot(plot, currentQuestionTurn());
    for (const QJsonObject& p : extra) logPlot(p);
    turns_.last().toolBlocks = blocks;

    Turn t;
    t.question = tr("(résultat du calcul demandé par l'assistant)");
    t.modelUser = forModel.join("\n\n");
    t.followUp = true;
    t.synthetic = true;
    t.view = newAssistantBubble();
    turns_.append(t);

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
    chat_->clearAll();
    if (clearFigures) figures_->clearCurves();
    emit requestReset();
    clearAttachment();
    statsLabel_->clear();
}

// ------------------------------------------------------------ pièces jointes, liens

void MainWindow::attachFile()
{
    const QString f = QFileDialog::getOpenFileName(
        this, tr("Joindre un fichier"), QString(),
        tr("Fichiers pris en charge (*.chr *.json *.html *.htm *.csv *.tsv *.txt *.md);;"
           "Résultats ChronoModel (*.html *.htm);;Tous les fichiers (*)"));
    if (f.isEmpty()) return;

    const QString ext = QFileInfo(f).suffix().toLower();
    if (QStringList{ "png", "jpg", "jpeg", "svg", "bmp", "gif", "webp", "tif", "tiff", "heic" }.contains(ext)) {
        // Le modèle ne reçoit que du texte : une image jointe ne serait pas vue (risque de réponse inventée).
        QMessageBox::information(this, tr("Joindre un fichier"),
                                 tr("Les images ne sont pas prises en charge : le modèle de langage ne reçoit que du texte.\n"
                                    "Joignez plutôt un tableau (CSV, TXT), un projet ChronoModel (.chr) ou ses résultats (.html)."));
        return;
    }
    QFile file(f);
    if (!file.open(QIODevice::ReadOnly)) { QMessageBox::warning(this, tr("Fichier"), file.errorString()); return; }
    const QByteArray data = file.readAll();

    QString text;
    bool chronoResults = false;
    if (ext == "html" || ext == "htm") {
        // Export des résultats de ChronoModel (Log_Posterior_Distrib_Stats.html…) : trop long pour le modèle, SeldonH en
        // extrait les valeurs utiles (convergence, σ des dates). Autre page HTML : son texte.
        const QString plain = chronoresults::htmlToText(QString::fromUtf8(data));
        if (chronoresults::looksLike(plain)) {
            text = chronoresults::summarize(chronoresults::parse(plain), QFileInfo(f).fileName());
            chronoResults = true;
        } else {
            text = plain;
        }
    }
    if (ext == "chr" || ext == "json") {
        const QJsonDocument d = QJsonDocument::fromJson(data);
        if (d.isObject() && d.object().contains("events")) text = ChrBuilder::summarize(d.object());
    }
    if (text.isEmpty()) {
        text = QString::fromUtf8(data);
        const int kMax = 8000;
        if (text.size() > kMax) text = text.left(kMax) + tr("\n[… fichier tronqué à %1 caractères]").arg(kMax);
    }
    attachName_ = QFileInfo(f).fileName();
    attachText_ = text;
    attachChronoResults_ = chronoResults;
    attachLabel_->setText(tr("📎 %1 (%2 caractères)").arg(attachName_).arg(text.size()));
    attachLabel_->show();
    attachClear_->show();
}

void MainWindow::clearAttachment()
{
    attachName_.clear();
    attachText_.clear();
    attachChronoResults_ = false;
    attachLabel_->hide();
    attachClear_->hide();
}

void MainWindow::onLink(const QUrl& url)
{
    if (url.scheme() != "seldon") { QDesktopServices::openUrl(url); return; }
    const QString action = url.host();
    const int idx = url.path().mid(1).toInt();
    if (action == "choose-model") chooseModel();
    else if (action == "open-models") openModelsFolder();
    else if (action == "help") showHelp();
    else if (action == "insert") insertExample(QUrlQuery(url).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded));
    else if (action == "chr") saveChrFromTurn(idx);
    else if (action == "continue") continueAnswer(idx);
    else if (action == "figure") showFigure(idx);
    else if (action == "densityfile") saveDensityFile(url);
    else if (action == "copy" && idx >= 0 && idx < turns_.size()) QApplication::clipboard()->setText(turns_[idx].answer);
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
    const ChrResult r = ChrBuilder::build(spec);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Modèle ChronoModel"),
                             tr("Le modèle proposé est incomplet :\n\n• %1\n\nDemandez à l'assistant de le corriger.").arg(r.errors.join("\n• ")));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Enregistrer le modèle ChronoModel"),
        QDir::homePath() + "/" + r.suggestedName + ".chr", tr("Projet ChronoModel (*.chr)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { QMessageBox::warning(this, tr("Enregistrement"), f.errorString()); return; }
    f.write(QJsonDocument(r.chr).toJson(QJsonDocument::Indented));
    f.close();

    QString html = tr("Modèle enregistré : <code>%1</code> (%2 événements).").arg(path.toHtmlEscaped()).arg(r.chr["events"].toArray().size());
    if (!r.warnings.isEmpty()) {
        html += "<ul>";
        for (const QString& w : r.warnings) html += "<li>" + w.toHtmlEscaped() + "</li>";
        html += "</ul>";
    }
    html += tr("<br><i>Fichier généré automatiquement : vérifiez-le à l'ouverture dans ChronoModel.</i>");
    logNotice(html);
}

void MainWindow::applyPlotBlocks(const QString& answer)
{
    static const QRegularExpression re("^```plot[ \\t]*\\n(.*?)^```",
                                       QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
    auto it = re.globalMatch(answer);
    while (it.hasNext()) {
        const QJsonDocument d = QJsonDocument::fromJson(it.next().captured(1).toUtf8());
        QString err;
        const bool ok = d.isObject() && logPlot(d.object(), &err);
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
    display_.append(d);
    return chat_->addAssistant();
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
    if (!t.answer.isEmpty())
        actions << QStringLiteral("<a href=\"seldon://copy/%1\">Copier</a>").arg(turn);
    if (!actions.isEmpty()) chat_->setActions(t.view, actions);
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
    for (const Turn& t : turns_) {
        StoredTurn s;
        s.question = t.question; s.modelUser = t.modelUser; s.answer = t.answer; s.toolBlocks = t.toolBlocks;
        s.skillIds = t.skillIds; s.followUp = t.followUp; s.synthetic = t.synthetic;
        c.turns.append(s);
    }
    c.display = display_;
    if (!c.hasContent()) return;
    for (const StoredTurn& s : c.turns)
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
    for (const StoredTurn& s : c.turns) {
        Turn t;
        t.question = s.question; t.modelUser = s.modelUser; t.answer = s.answer; t.toolBlocks = s.toolBlocks;
        t.skillIds = s.skillIds; t.followUp = s.followUp; t.synthetic = s.synthetic;
        turns_.append(t);
    }
    for (const DisplayItem& d : c.display) {
        switch (d.kind) {
        case DisplayItem::User:
            chat_->addUser(d.text);
            break;
        case DisplayItem::Notice:
            chat_->addNotice(d.text);
            break;
        case DisplayItem::Plot:
            if (figures_->addCurvesJson(d.plot)) recallFigure(d.plot);
            break;
        case DisplayItem::Assistant: {
            const int view = chat_->addAssistant();
            turns_[d.turn].view = view;
            chat_->setAssistant(view, d.text, d.thinking, false, true);
            setTurnActions(d.turn);
            break;
        }
        }
        display_.append(d);
    }
    history_->setItems(historyItems_, convId_);
    statusBar()->showMessage(tr("Conversation rouverte : %1. Vous pouvez la poursuivre.").arg(c.title), 6000);
}

void MainWindow::deleteConversation(const QString& id)
{
    if (generating_ && id == convId_) {
        statusBar()->showMessage(tr("Une réponse est en cours dans cette conversation : arrêtez-la avant de la supprimer."), 6000);
        return;
    }
    QString title = id;
    for (const ConversationInfo& i : historyItems_) if (i.id == id) title = i.title;
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

void MainWindow::saveTranscript()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Enregistrer la conversation"),
                                                      QDir::homePath() + "/conversation_seldonh.html", tr("Page HTML (*.html)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(chat_->exportHtml().toUtf8());
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
    for (const char* m : { "c14", "f14c", "am" })
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
    const QString cmDir = AppSettings::chronoModelCalibDir();
    auto origin = [&](const QString& path) {
        if (path.startsWith(QLatin1String(":/"))) return tr("intégrée");
        if (!cmDir.isEmpty() && path.startsWith(cmDir)) return tr("ChronoModel");
        return tr("utilisateur");
    };
    QString html = tr("<b>Courbes de référence</b><br><i>Ordre de recherche : votre dossier (%1), puis ChronoModel (%2), "
                      "puis les courbes intégrées. Une courbe se désigne par son nom de fichier sans extension.</i>")
                       .arg(settings_.curvesDir.toHtmlEscaped(),
                            settings_.useChronoModelCurves ? cmDir.toHtmlEscaped() : tr("désactivé"));
    const struct { const char* id; const char* title; } methods[] = {
        { "c14", "Radiocarbone (14C, fichiers .14c)" }, { "f14c", "F14C post-bombe (fichiers .14c)" },
        { "am", "Archéomagnétisme (fichiers .ref)" } };
    // Une courbe par ligne (liste à puces), avec son origine.
    for (const auto& m : methods) {
        const QStringList names = CurveStore::names(QString::fromLatin1(m.id));
        html += QStringLiteral("<p><b>%1</b> — %2</p><ul>").arg(tr(m.title), tr("%n courbe(s)", nullptr, int(names.size())));
        for (const QString& n : names)
            html += QStringLiteral("<li><code>%1</code> <i>(%2)</i></li>")
                        .arg(n.toHtmlEscaped(), origin(CurveStore::sourceOf(QString::fromLatin1(m.id), n)));
        if (names.isEmpty()) html += QStringLiteral("<li><i>%1</i></li>").arg(tr("aucune"));
        html += QStringLiteral("</ul>");
    }
    logNotice(html);
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
    statusBar()->showMessage(tr("%1 compétences chargées").arg(n), 4000);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("À propos de SeldonH"),
                       tr("<b>SeldonH</b><br>Assistant local d'aide à la datation et à la modélisation chronologique.<br>"
                          "Moteur : llama.cpp (exécution locale).<br>%1 compétences chargées.").arg(skills_.skills().size()));
}
