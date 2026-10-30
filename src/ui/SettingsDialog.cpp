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

#include "SettingsDialog.h"

#include "../core/AutoTune.h"
#include "../core/ChatText.h"
#include "../core/SecretStore.h"
#include "../llm/RemoteApi.h"
#include "../methods/common/DateFormat.h"

#include <QCheckBox>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>

#include <algorithm>

namespace seldon {

SettingsDialog::SettingsDialog(const AppSettings& s, QWidget* parent) : QDialog(parent), base_(s)
{
    setWindowTitle(tr("Préférences"));
    setMinimumWidth(780);
    setSizeGripEnabled(true);
    nam_ = new QNetworkAccessManager(this);

    auto* outer = new QVBoxLayout(this);
    form_ = new QFormLayout;
    // Les champs occupent toute la largeur disponible (sous macOS, le défaut les laisse à leur taille minimale).
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form_->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form_->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    outer->addLayout(form_);
    QFormLayout* form = form_;

    auto expanding = [](QWidget* w) { w->setSizePolicy(QSizePolicy::Expanding, w->sizePolicy().verticalPolicy()); };
    auto row = [this](QLineEdit*& edit, const QString& text, void (SettingsDialog::*slot)()) {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        edit = new QLineEdit(text);
        edit->setMinimumWidth(380);
        edit->setCursorPosition(0);
        edit->setToolTip(text);
        auto* b = new QPushButton(tr("Parcourir…"));
        h->addWidget(edit, 1);
        h->addWidget(b);
        connect(b, &QPushButton::clicked, this, slot);
        return w;
    };

    // --- Choix du moteur : local ou distant
    auto* kindRow = new QWidget;
    auto* kindLay = new QHBoxLayout(kindRow);
    kindLay->setContentsMargins(0, 0, 0, 0);
    localRadio_ = new QRadioButton(tr("Modèle local (fichier .gguf, tout reste sur cet ordinateur)"));
    remoteRadio_ = new QRadioButton(tr("Modèle distant (API compatible OpenAI)"));
    (s.useRemote ? remoteRadio_ : localRadio_)->setChecked(true);
    kindLay->addWidget(localRadio_);
    kindLay->addWidget(remoteRadio_);
    kindLay->addStretch();
    form->addRow(tr("Moteur"), kindRow);

    // --- local
    modelRow_ = row(model_, s.modelPath, &SettingsDialog::browseModel);
    form->addRow(tr("Modèle (.gguf)"), modelRow_);

    ctx_ = new QComboBox;
    for (int v : { 2048, 4096, 8192, 16384, 32768 }) ctx_->addItem(tr("%1 tokens").arg(v), v);
    ctx_->setCurrentIndex(std::max(0, ctx_->findData(s.nCtx)));
    expanding(ctx_);
    form->addRow(tr("Taille du contexte"), ctx_);

    threads_ = new QSpinBox;
    threads_->setRange(0, QThread::idealThreadCount());
    threads_->setSpecialValueText(tr("Automatique"));
    threads_->setValue(s.nThreads);
    form->addRow(tr("Threads CPU"), threads_);

#ifdef Q_OS_MACOS
    gpu_ = new QCheckBox(tr("Utiliser le GPU (Metal)"));
#else
    gpu_ = new QCheckBox(tr("Utiliser le GPU (Vulkan ou CUDA, selon la version installée)"));
#endif
    gpu_->setChecked(s.nGpuLayers != 0);
    form->addRow(QString(), gpu_);

    // --- distant
    remoteUrl_ = new QLineEdit(s.remoteUrl);
    remoteUrl_->setPlaceholderText(QStringLiteral("https://api.openai.com/v1"));
    remoteUrl_->setToolTip(tr("Adresse de base de l'API, généralement terminée par /v1. Exemples : https://api.openai.com/v1, "
                              "https://api.mistral.ai/v1, https://openrouter.ai/api/v1, http://localhost:11434/v1 (Ollama), "
                              "http://localhost:1234/v1 (LM Studio)."));
    form->addRow(tr("Base URL"), remoteUrl_);
    httpWarning_ = new QLabel(tr("<span style='color:#b35c00'>⚠ Connexion non chiffrée (http://) : la clé API transiterait en clair "
                                 "sur le réseau. Utilisez une adresse en https://.</span>"));
    httpWarning_->setObjectName(QStringLiteral("httpWarning"));
    httpWarning_->setWordWrap(true);
    form->addRow(QString(), httpWarning_);

    remoteKeyRow_ = new QWidget;
    auto* keyLay = new QHBoxLayout(remoteKeyRow_);
    keyLay->setContentsMargins(0, 0, 0, 0);
    remoteKey_ = new QLineEdit(s.remoteKey);
    remoteKey_->setEchoMode(QLineEdit::Password);
    remoteKey_->setPlaceholderText(tr("Clé API (laisser vide pour un serveur local sans clé)"));
    remoteKey_->setToolTip(tr("La clé est conservée dans le trousseau du système (voir ci-dessous). Vous pouvez aussi la fournir "
                              "par la variable d'environnement SELDONH_API_KEY (champ laissé vide)."));
    showKey_ = new QCheckBox(tr("Afficher"));
    clearKey_ = new QPushButton(tr("Effacer"));
    clearKey_->setToolTip(tr("Vide le champ ; la clé est retirée du trousseau en validant par OK."));
    keyLay->addWidget(remoteKey_, 1);
    keyLay->addWidget(showKey_);
    keyLay->addWidget(clearKey_);
    connect(showKey_, &QCheckBox::toggled, this, [this](bool on) {
        remoteKey_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(clearKey_, &QPushButton::clicked, remoteKey_, &QLineEdit::clear);
    form->addRow(tr("Clé API"), remoteKeyRow_);
    keyStore_ = new QLabel(secrets::isSecure()
                               ? tr("<span style='color:#1b7f3b'>🔒 %1</span>").arg(secrets::storageDescription().toHtmlEscaped())
                               : tr("<span style='color:#b35c00'>⚠ %1</span>").arg(secrets::storageDescription().toHtmlEscaped()));
    keyStore_->setObjectName(QStringLiteral("keyStore"));
    keyStore_->setWordWrap(true);
    form->addRow(QString(), keyStore_);
    connect(remoteUrl_, &QLineEdit::textChanged, this, &SettingsDialog::updateHttpWarning);
    connect(remoteKey_, &QLineEdit::textChanged, this, &SettingsDialog::updateHttpWarning);

    remoteModelRow_ = new QWidget;
    auto* rmLay = new QHBoxLayout(remoteModelRow_);
    rmLay->setContentsMargins(0, 0, 0, 0);
    remoteModel_ = new QComboBox;
    remoteModel_->setEditable(true);
    remoteModel_->setInsertPolicy(QComboBox::NoInsert);
    remoteModel_->setMinimumWidth(300);
    remoteModel_->setEditText(s.remoteModel);
    remoteModel_->setToolTip(tr("Nom du modèle chez le fournisseur (ex. gpt-4o-mini, mistral-small-latest, qwen3:8b). "
                                "« Lister les modèles » interroge le serveur."));
    expanding(remoteModel_);
    listBtn_ = new QPushButton(tr("Lister les modèles"));
    rmLay->addWidget(remoteModel_, 1);
    rmLay->addWidget(listBtn_);
    connect(listBtn_, &QPushButton::clicked, this, &SettingsDialog::listRemoteModels);
    form->addRow(tr("Nom du modèle"), remoteModelRow_);

    {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        remoteToolsMode_ = new QComboBox;
        remoteToolsMode_->addItem(tr("Automatique (selon le test du serveur)"), QStringLiteral("auto"));
        remoteToolsMode_->addItem(tr("Toujours"), QStringLiteral("on"));
        remoteToolsMode_->addItem(tr("Jamais (bloc de calcul dans la réponse)"), QStringLiteral("off"));
        remoteToolsMode_->setCurrentIndex(std::max(0, remoteToolsMode_->findData(s.remoteToolsMode)));
        remoteToolsMode_->setToolTip(tr("Avec l'appel d'outils natif, le modèle distant demande lui-même un calcul (calibration, HPD…) ; "
                                        "SeldonH l'exécute sur cet ordinateur avec les méthodes de ChronoModel et lui renvoie les valeurs. "
                                        "Tous les serveurs ne le permettent pas : en mode automatique, SeldonH envoie une requête de test "
                                        "(un outil factice) et retient le résultat pour ce serveur et ce modèle. Sans appel d'outils, les "
                                        "calculs passent par un bloc de calcul écrit dans la réponse."));
        probeBtn_ = new QPushButton(tr("Tester"));
        probeBtn_->setToolTip(tr("Envoie au serveur une requête de test avec un outil factice et indique si le modèle l'appelle. "
                                 "Le résultat est retenu pour ce serveur et ce modèle."));
        h->addWidget(remoteToolsMode_, 1);
        h->addWidget(probeBtn_);
        form->addRow(tr("Appel d'outils"), w);
        remoteToolsRow_ = w;
        connect(probeBtn_, &QPushButton::clicked, this, &SettingsDialog::testTools);
    }
    probeStatus_ = new QLabel;
    probeStatus_->setObjectName(QStringLiteral("probeStatus"));
    probeStatus_->setWordWrap(true);
    form->addRow(QString(), probeStatus_);
    probe_ = new ToolsProbe(this);
    connect(probe_, &ToolsProbe::finished, this, [this](const ToolProbeResult& r) {
        probeBtn_->setEnabled(true);
        probeStatus_->setText(toolprobe::describe(r));
        updateBackendRows();
    });
    connect(remoteUrl_, &QLineEdit::textChanged, this, &SettingsDialog::showCachedProbe);
    connect(remoteModel_, &QComboBox::currentTextChanged, this, &SettingsDialog::showCachedProbe);
    showCachedProbe();
    remoteStatus_ = new QLabel;
    remoteStatus_->setWordWrap(true);
    form->addRow(QString(), remoteStatus_);
    remoteWarning_ = new QLabel(tr("<b>Confidentialité :</b> avec un modèle distant, vos questions, les compétences ajoutées "
                                   "et les fichiers joints sont envoyés au serveur indiqué. Les calculs de datation, eux, restent "
                                   "faits sur cet ordinateur."));
    remoteWarning_->setWordWrap(true);
    form->addRow(QString(), remoteWarning_);

    // --- commun
    optimizeBtn_ = new QPushButton(tr("Optimiser pour ce modèle"));
    optimizeBtn_->setToolTip(tr("Propose la taille du contexte (modèle local), la longueur max. d'une réponse et le budget des "
                                "compétences d'après le modèle (contexte, taille), la mémoire de l'ordinateur et la vitesse "
                                "mesurée. Les champs sont remplis : vérifiez puis validez par OK."));
    optimizeStatus_ = new QLabel;
    optimizeStatus_->setObjectName(QStringLiteral("optimizeStatus"));
    optimizeStatus_->setWordWrap(true);
    {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(optimizeBtn_);
        h->addStretch();
        form->addRow(tr("Réglages automatiques"), w);
    }
    form->addRow(QString(), optimizeStatus_);
    connect(optimizeBtn_, &QPushButton::clicked, this, &SettingsDialog::optimize);

    temp_ = new QDoubleSpinBox;
    temp_->setRange(0.0, 2.0);
    temp_->setSingleStep(0.1);
    temp_->setValue(s.temperature);
    temp_->setToolTip(tr("0 = réponses déterministes ; 0.3-0.7 recommandé pour des données chiffrées."));
    form->addRow(tr("Température"), temp_);

    maxTokens_ = new QSpinBox;
    maxTokens_->setRange(64, 8192);
    maxTokens_->setSingleStep(100);
    maxTokens_->setValue(s.maxTokens);
    maxTokens_->setSuffix(tr(" tokens"));
    maxTokens_->setToolTip(tr("Au-delà, la réponse est coupée : SeldonH le signale et propose « Continuer la réponse ». "
                              "En mode réflexion (modèle local), %1 tokens supplémentaires sont réservés à la réflexion. "
                              "Avec un modèle distant de raisonnement, son raisonnement caché compte aussi dans cette limite.")
                               .arg(AppSettings::kThinkingReserve));
    form->addRow(tr("Longueur max. d'une réponse"), maxTokens_);

    thinking_ = new QCheckBox(tr("Mode réflexion (modèle local ; plus lent)"));
    thinking_->setToolTip(tr("Le modèle raisonne avant de répondre : plus lent, utile pour les cas complexes. "
                             "Sans effet avec un modèle distant (choisissez alors un modèle de raisonnement chez le fournisseur)."));
    thinking_->setChecked(s.thinking);
    form->addRow(QString(), thinking_);

    skillChars_ = new QSpinBox;
    skillChars_->setRange(1000, 20000);
    skillChars_->setSingleStep(500);
    skillChars_->setSuffix(tr(" caractères"));
    skillChars_->setValue(s.skillChars);
    skillChars_->setToolTip(tr("Quantité maximale de connaissances (compétences) ajoutées à chaque question."));
    form->addRow(tr("Budget des compétences"), skillChars_);

    form->addRow(tr("Dossier des compétences"), row(skillsDir_, s.skillsDir, &SettingsDialog::browseSkills));

    dateFormat_ = new QComboBox;
    for (DateFormat f : dateformat::all())
        dateFormat_->addItem(dateformat::title(f), QString::fromLatin1(dateformat::id(f)));
    dateFormat_->setCurrentIndex(std::max(0, dateFormat_->findData(s.dateFormat)));
    dateFormat_->setToolTip(tr("Format d'affichage des dates dans les fiches de résultat et les graphiques, comme dans ChronoModel. "
                               "Une demande explicite dans la question (« donne le résultat en cal BP ») le remplace pour cette question."));
    expanding(dateFormat_);
    form->addRow(tr("Format des dates"), dateFormat_);

    decimals_ = new QSpinBox;
    decimals_->setRange(dateformat::kAutoDecimals, dateformat::kMaxDecimals);
    decimals_->setSpecialValueText(tr("Automatique"));          // valeur -1
    decimals_->setValue(s.decimals);
    decimals_->setToolTip(tr("Nombre de décimales des dates, intervalles et écarts-types dans les fiches de résultat. "
                             "« Automatique » adapte la précision à l'incertitude de chaque résultat (2 chiffres significatifs "
                             "sur l'écart-type, dans l'unité choisie) : à l'année près pour une datation à ± 30 ans, "
                             "0,001 ka pour une incertitude de 0,02 ka."));
    form->addRow(tr("Décimales"), decimals_);

    userPrompt_ = new QPlainTextEdit(s.userPrompt);
    userPrompt_->setPlaceholderText(tr("Consignes ajoutées à chaque question, par exemple :\n"
                                       "• Réponds en trois phrases au maximum.\n"
                                       "• Donne les âges en BP.\n"
                                       "• Tutoie-moi / vouvoie-moi."));
    userPrompt_->setFixedHeight(QFontMetrics(userPrompt_->font()).lineSpacing() * 5 + 12);
    userPrompt_->setToolTip(tr("Ces consignes s'appliquent à toutes les réponses. Elles ne modifient pas les résultats de calcul de SeldonH. "
                               "Maximum 1500 caractères."));
    form->addRow(tr("Consignes personnelles"), userPrompt_);

    keepHistory_ = new QCheckBox(tr("Enregistrer automatiquement les conversations (volet Historique)"));
    keepHistory_->setChecked(s.keepHistory);
    keepHistory_->setToolTip(tr("Les questions, les réponses, les fiches de calcul et les courbes sont gardées dans un dossier de données de "
                                "SeldonH, sur cet ordinateur, en clair. Les fichiers joints y figurent avec leur contenu. Décochez pour ne rien "
                                "conserver ; les conversations déjà enregistrées restent dans le volet Historique jusqu'à leur suppression."));
    form->addRow(QString(), keepHistory_);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(box);

    connect(localRadio_, &QRadioButton::toggled, this, &SettingsDialog::updateBackendRows);
    connect(remoteRadio_, &QRadioButton::toggled, this, &SettingsDialog::updateBackendRows);
    ready_ = true;
    updateBackendRows();
    resize(std::max(880, sizeHint().width()), sizeHint().height());
}

void SettingsDialog::updateBackendRows()
{
    const bool remote = remoteRadio_->isChecked();
    for (QWidget* w : { static_cast<QWidget*>(modelRow_), static_cast<QWidget*>(ctx_), static_cast<QWidget*>(threads_), static_cast<QWidget*>(gpu_) })
        form_->setRowVisible(w, !remote);
    for (QWidget* w : { static_cast<QWidget*>(remoteUrl_), static_cast<QWidget*>(remoteKeyRow_), static_cast<QWidget*>(keyStore_),
                        static_cast<QWidget*>(remoteModelRow_), static_cast<QWidget*>(remoteToolsRow_), static_cast<QWidget*>(remoteWarning_) })
        form_->setRowVisible(w, remote);
    form_->setRowVisible(httpWarning_, remote && remote::sendsKeyInClear(remoteUrl_->text(), remoteKey_->text()));
    form_->setRowVisible(remoteStatus_, remote && !remoteStatus_->text().isEmpty());
    form_->setRowVisible(probeStatus_, remote && !probeStatus_->text().isEmpty());
    form_->setRowVisible(optimizeStatus_, !optimizeStatus_->text().isEmpty());
    thinking_->setEnabled(!remote);
}

void SettingsDialog::updateHttpWarning()
{
    form_->setRowVisible(httpWarning_, remoteRadio_->isChecked()
                                           && remote::sendsKeyInClear(remoteUrl_->text(), remoteKey_->text()));
}

// OK : si la clé API partirait en clair, on demande confirmation (un serveur du réseau du laboratoire en http://
// peut être un choix volontaire) ; « Non » laisse la boîte ouverte pour corriger l'adresse.
void SettingsDialog::done(int r)
{
    if (r == QDialog::Accepted && remoteRadio_->isChecked()
        && remote::sendsKeyInClear(remoteUrl_->text(), remoteKey_->text())) {
        const QString host = remote::hostOf(remoteUrl_->text());
        const auto ans = QMessageBox::warning(
            this, tr("Connexion non chiffrée"),
            tr("L'adresse « %1 » utilise http:// : la clé API et vos questions circuleraient en clair sur le réseau "
               "et pourraient être lues par un tiers.\n\nEnregistrer quand même ?").arg(host),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ans != QMessageBox::Yes) { remoteUrl_->setFocus(); return; }
    }
    QDialog::done(r);
}

// Résultat déjà connu du test d'appel d'outils pour la Base URL et le modèle affichés.
void SettingsDialog::showCachedProbe()
{
    if (probe_ && probe_->running()) return;
    ToolProbeResult r;
    probeStatus_->setText(toolprobe::cached(remoteUrl_->text(), remoteModel_->currentText(), r)
                              ? toolprobe::describe(r)
                              : tr("<small>Appel d'outils pas encore testé pour ce serveur et ce modèle (test automatique à la "
                                   "première utilisation, ou bouton « Tester »).</small>"));
    if (ready_) updateBackendRows();
}

void SettingsDialog::testTools()
{
    RemoteConfig cfg;
    cfg.baseUrl = remoteUrl_->text().trimmed();
    cfg.apiKey  = remoteKey_->text().trimmed();
    cfg.model   = remoteModel_->currentText().trimmed();
    if (!cfg.isValid()) { probeStatus_->setText(tr("Renseignez d'abord la Base URL et le nom du modèle.")); updateBackendRows(); return; }
    probeBtn_->setEnabled(false);
    probeStatus_->setText(tr("Test de l'appel d'outils sur %1…").arg(remote::hostOf(cfg.baseUrl).toHtmlEscaped()));
    updateBackendRows();
    probe_->start(cfg);
}

// GET {Base URL}/models : vérifie l'adresse et la clé, et propose les noms de modèles disponibles.
void SettingsDialog::listRemoteModels()
{
    const QString base = remoteUrl_->text().trimmed();
    if (base.isEmpty()) { remoteStatus_->setText(tr("Renseignez d'abord la Base URL.")); updateBackendRows(); return; }
    QNetworkRequest rq(remote::modelsUrl(base));
    const QString key = remoteKey_->text().trimmed();
    if (!key.isEmpty()) rq.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    rq.setTransferTimeout(15000);
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // HTTP/1.1 : la pile HTTP/2 de Qt 6 lit le socket TLS après sa fermeture par le serveur (fin du flux SSE,
    // connexion inactive) et affiche « QIODevice::read (QSslSocket): device not open ». Gain d'HTTP/2 nul ici.
    rq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    listBtn_->setEnabled(false);
    remoteStatus_->setText(tr("Connexion à %1…").arg(remote::hostOf(base).toHtmlEscaped()));
    updateBackendRows();
    QNetworkReply* r = nam_->get(rq);
    connect(r, &QNetworkReply::finished, this, [this, r, base] {
        listBtn_->setEnabled(true);
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->isOpen() ? r->readAll() : QByteArray();
        remoteLimits_ = remote::parseModelLimits(body);
        if (r->error() != QNetworkReply::NoError && status == 0) {
            remoteStatus_->setText(tr("<span style='color:#b00020'>Connexion impossible : %1</span>").arg(r->errorString().toHtmlEscaped()));
        } else if (status >= 400) {
            remoteStatus_->setText(tr("<span style='color:#b00020'>%1</span>").arg(remote::errorMessage(body, status).toHtmlEscaped()));
        } else {
            const QStringList ids = remote::parseModelList(body);
            const QString current = remoteModel_->currentText();
            remoteModel_->clear();
            remoteModel_->addItems(ids);
            remoteModel_->setEditText(current.isEmpty() && !ids.isEmpty() ? ids.first() : current);
            remoteStatus_->setText(ids.isEmpty() ? tr("Connexion réussie, mais le serveur ne liste aucun modèle : saisissez son nom à la main.")
                                                 : tr("<span style='color:#1b7f3b'>Connexion réussie : %1 modèle(s) disponibles.</span>").arg(ids.size()));
        }
        updateBackendRows();
        r->deleteLater();
        if (optimizeAfterList_) { optimizeAfterList_ = false; applyOptimization(); }
    });
}

// Modèle distant : les limites viennent de la liste /models (relue si besoin) ; modèle local : du modèle chargé.
void SettingsDialog::optimize()
{
    if (remoteRadio_->isChecked()) {
        if (!remoteLimits_.contains(remoteModel_->currentText().trimmed()) && !remoteUrl_->text().trimmed().isEmpty()) {
            optimizeAfterList_ = true;
            optimizeStatus_->setText(tr("Interrogation du serveur…"));
            updateBackendRows();
            listRemoteModels();
            return;
        }
    }
    applyOptimization();
}

void SettingsDialog::applyOptimization()
{
    TuneInput in;
    in.remote   = remoteRadio_->isChecked();
    in.thinking = thinking_->isChecked();
    if (in.remote) {
        const remote::ModelLimits lim = remoteLimits_.value(remoteModel_->currentText().trimmed());
        in.remoteContext   = lim.context;
        in.remoteMaxOutput = lim.maxOutput;
    } else {
        // Les caractéristiques ne sont connues qu'une fois le modèle chargé.
        if (localInfo_.description.empty() || model_->text().trimmed() != base_.modelPath) {
            optimizeStatus_->setText(tr("<span style='color:#b35c00'>Chargez d'abord ce modèle (validez par OK), puis relancez "
                                        "l'optimisation.</span>"));
            updateBackendRows();
            return;
        }
        in.trainContext = localInfo_.trainContext;
        in.nLayer = localInfo_.nLayer; in.nEmbd = localInfo_.nEmbd;
        in.nHead = localInfo_.nHead;   in.nHeadKv = localInfo_.nHeadKv;
        in.modelBytes = localInfo_.modelBytes;
        in.memTotal   = localInfo_.memTotal;
        in.onGpu      = localInfo_.onGpu && gpu_->isChecked();
        in.tokPerSec  = localTps_;
    }
    const TuneResult r = autoTune(in);

    QStringList set;
    if (r.nCtx > 0) {
        const int i = ctx_->findData(r.nCtx);
        if (i >= 0) ctx_->setCurrentIndex(i);
        set << tr("contexte %1").arg(r.nCtx);
    }
    maxTokens_->setValue(r.maxTokens);
    skillChars_->setValue(r.skillChars);
    set << tr("réponse max. %1 tokens").arg(r.maxTokens) << tr("compétences %1 caractères").arg(r.skillChars);
    QString html = tr("<b>Proposé :</b> %1. Vérifiez puis validez par OK.").arg(set.join(QStringLiteral(" · ")));
    if (!in.remote && localTps_ <= 0)
        html += tr("<br><i>Posez une question puis relancez l'optimisation pour tenir compte de la vitesse réelle.</i>");
    for (const QString& n : r.notes) html += QStringLiteral("<br><small>%1</small>").arg(n.toHtmlEscaped());
    optimizeStatus_->setText(html);
    updateBackendRows();
}

AppSettings SettingsDialog::settings() const
{
    AppSettings a = base_;
    a.modelPath   = model_->text().trimmed();
    a.nCtx        = ctx_->currentData().toInt();
    a.nThreads    = threads_->value();
    a.nGpuLayers  = gpu_->isChecked() ? 999 : 0;
    a.temperature = temp_->value();
    a.maxTokens   = maxTokens_->value();
    a.thinking    = thinking_->isChecked();
    a.skillChars  = skillChars_->value();
    a.skillsDir   = skillsDir_->text().trimmed();
    a.dateFormat  = dateFormat_->currentData().toString();
    a.decimals    = decimals_->value();
    a.useRemote   = remoteRadio_->isChecked();
    a.remoteUrl   = remoteUrl_->text().trimmed();
    a.remoteKey   = remoteKey_->text().trimmed();
    a.remoteModel = remoteModel_->currentText().trimmed();
    a.remoteToolsMode = remoteToolsMode_->currentData().toString();
    a.userPrompt  = QString::fromStdString(trimUserPrompt(userPrompt_->toPlainText().toStdString()));
    a.keepHistory = keepHistory_->isChecked();
    return a;
}

void SettingsDialog::browseModel()
{
    const QString f = QFileDialog::getOpenFileName(this, tr("Choisir un modèle GGUF"), model_->text(),
                                                   tr("Modèles GGUF (*.gguf)"));
    if (!f.isEmpty()) model_->setText(f);
}

void SettingsDialog::browseSkills()
{
    const QString d = QFileDialog::getExistingDirectory(this, tr("Dossier des compétences"), skillsDir_->text());
    if (!d.isEmpty()) skillsDir_->setText(d);
}

} // namespace seldon
