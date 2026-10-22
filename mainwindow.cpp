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
#include <QJsonObject>

#include "src/core/ChatText.h"
#include "src/core/ChrBuilder.h"
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
    attachBtn_->setText(QStringLiteral("📎"));
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

    auto* buttons = new QVBoxLayout;
    buttons->addWidget(sendBtn_);
    buttons->addWidget(stopBtn_);
    buttons->addStretch();

    auto* inputRow = new QHBoxLayout;
    inputRow->addWidget(attachBtn_, 0, Qt::AlignTop);
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
    history_ = new HistoryPanel;
    outer_ = new QSplitter(Qt::Horizontal);
    outer_->addWidget(history_);
    outer_->addWidget(splitter_);
    outer_->setStretchFactor(0, 0);
    outer_->setStretchFactor(1, 1);
    outer_->setCollapsible(0, false);
    outer_->setCollapsible(1, false);
    outer_->setSizes({ 270, 1010 });
    history_->setMinimumWidth(200);

    historyStrip_ = new CollapseStrip(tr("Historique des conversations"));
    historyStrip_->setToolTip(tr("Afficher ou replier l'historique des conversations (Ctrl+Maj+H)"));

    auto* central = new QWidget;
    auto* centralLay = new QHBoxLayout(central);
    centralLay->setContentsMargins(0, 0, 0, 0);
    centralLay->setSpacing(0);
    centralLay->addWidget(historyStrip_);
    centralLay->addWidget(outer_, 1);
    setCentralWidget(central);
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

    QMenu* view = menuBar()->addMenu(tr("&Affichage"));
    QAction* fig = view->addAction(tr("Panneau graphiques"));
    figAction_ = fig;
    fig->setCheckable(true);
    fig->setChecked(true);
    fig->setShortcut(QKeySequence(Qt::Key_F4));
    connect(fig, &QAction::toggled, this, [this](bool on) { splitter_->widget(1)->setVisible(on); });
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
    if (figAction_ && !figAction_->isChecked()) figAction_->setChecked(true);      // réaffiche le panneau de droite
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
    const QStringList pinned = MethodRunner::isModelRequest(q) ? QStringList{ QStringLiteral("03_chronomodel_creer_modele") } : QStringList();
    const QList<SkillMatch> matches = skills_.select(q, settings_.skillChars, recentSkillIds(3), pinned);
    const QString knowledge = SkillManager::format(matches);

    Turn t;
    t.question = q;
    for (const SkillMatch& m : matches) t.skillIds << m.skill->id;

    // Calculs déterministes (calibration 14C) : faits par l'application, pas par le modèle.
    const RunOutcome dating = MethodRunner::analyzeQuestion(q, dateformat::fromId(settings_.dateFormat));
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
        if (!dating.plot.isEmpty()) logPlot(dating.plot);
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
    sp.maxTokens      = settings_.maxTokens;
    sp.enableThinking = settings_.thinking;
    if (remoteActive()) emit requestRemoteGenerate(buildChat(), sp);
    else emit requestGenerate(buildChat(), sp);
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
    if (!out.plot.isEmpty()) logPlot(out.plot);

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
    chat_->setAssistant(turns_.last().view, QString::fromStdString(s.answer),
                        QString::fromStdString(s.thinking), s.thinkingOpen, false);
}

void MainWindow::finishAssistantMessage(bool interrupted)
{
    if (turns_.isEmpty()) return;
    Turn& t = turns_.last();
    const SplitText s = splitThinking(raw_.toStdString(), startsInThink_);
    QString answer = QString::fromStdString(s.answer);
    t.answer = answer;
    if (interrupted) answer += tr("\n\n*(réponse interrompue)*");
    chat_->setAssistant(t.view, answer, QString::fromStdString(s.thinking), false, true);
    logAssistantText(turns_.size() - 1, answer, QString::fromStdString(s.thinking));
    setTurnActions(turns_.size() - 1);

    applyPlotBlocks(t.answer);
}

void MainWindow::onFinished(const GenStats& st)
{
    generating_ = false;
    finishAssistantMessage(st.stoppedByUser);
    saveCurrent();

    const double tps = st.genMs > 0 ? st.generated * 1000.0 / st.genMs : 0.0;
    QString msg = tr("%1 tokens générés — %2 tok/s").arg(st.generated).arg(tps, 0, 'f', 1);
    if (st.droppedMessages > 0)
        msg += tr(" — %1 ancien(s) message(s) oublié(s) (contexte plein)").arg(st.droppedMessages);
    if (st.contextFull) msg += tr(" — contexte saturé");

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

    QStringList called;
    for (const ToolCall& c : calls) called << "`" + c.name + "`";
    if (turns_.last().answer.trimmed().isEmpty()) {
        const QString shown = tr("*🔧 Outil de calcul appelé : %1*").arg(called.join(", "));
        chat_->setAssistant(turns_.last().view, shown, QString(), false, true);
        logAssistantText(turns_.size() - 1, shown, QString());
    }

    const DateFormat fmt = displayFormat();
    QVector<ToolResult> results;
    QVector<QJsonObject> plots;
    QStringList forModel;
    QString blocks;
    for (const ToolCall& c : calls) {
        const tools::ToolOutcome o = tools::execute(c.name, c.arguments, fmt);
        results.append({ c.id, c.name, o.content });
        if (o.executed) {
            logNotice(o.run.noticeHtml);
            plots << o.run.plot;
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
    if (!plot.isEmpty()) logPlot(plot);
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
        tr("Fichiers pris en charge (*.chr *.json *.csv *.tsv *.txt *.md *.png *.jpg *.jpeg *.svg);;Tous les fichiers (*)"));
    if (f.isEmpty()) return;

    const QString ext = QFileInfo(f).suffix().toLower();
    if (QStringList{ "png", "jpg", "jpeg", "svg", "bmp", "gif" }.contains(ext)) {
        if (!figures_->addImageFile(f)) QMessageBox::warning(this, tr("Image"), tr("Image illisible."));
        return;
    }
    QFile file(f);
    if (!file.open(QIODevice::ReadOnly)) { QMessageBox::warning(this, tr("Fichier"), file.errorString()); return; }
    const QByteArray data = file.readAll();

    QString text;
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
    attachLabel_->setText(tr("📎 %1 (%2 caractères)").arg(attachName_).arg(text.size()));
    attachLabel_->show();
    attachClear_->show();
}

void MainWindow::clearAttachment()
{
    attachName_.clear();
    attachText_.clear();
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

bool MainWindow::logPlot(const QJsonObject& plot, QString* err)
{
    if (!figures_->addCurvesJson(plot, err)) return false;
    DisplayItem d; d.kind = DisplayItem::Plot; d.plot = plot;
    display_.append(d);
    return true;
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
    if (ChrBuilder::extractSpec(t.answer, spec))
        actions << QStringLiteral("<a href=\"seldon://chr/%1\">💾 Enregistrer le modèle (.chr)</a>").arg(turn);
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
            figures_->addCurvesJson(d.plot);
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
