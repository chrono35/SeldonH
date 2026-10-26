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

#pragma once

#include <QDialog>

#include "../core/AppSettings.h"
#include "../llm/LlmCore.h"
#include "../llm/RemoteApi.h"

#include <QHash>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace seldon {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const AppSettings& s, QWidget* parent = nullptr);
    AppSettings settings() const;
    // Modèle local chargé et vitesse mesurée sur sa dernière réponse (0 = inconnue) : utilisés par « Optimiser ».
    void setLocalModel(const ModelInfo& info, double tokPerSec) { localInfo_ = info; localTps_ = tokPerSec; }

public slots:
    void done(int r) override;     // OK : confirmation si la clé API partirait en clair (http://)

private slots:
    void browseModel();
    void browseSkills();
    void updateBackendRows();
    void listRemoteModels();
    void updateHttpWarning();
    void optimize();                // « Optimiser pour ce modèle »

private:
    AppSettings     base_;
    QFormLayout*    form_;
    QRadioButton*   localRadio_;
    QRadioButton*   remoteRadio_;
    // local
    QLineEdit*      model_;
    QWidget*        modelRow_;
    QComboBox*      ctx_;
    QSpinBox*       threads_;
    QCheckBox*      gpu_;
    // distant
    QLineEdit*      remoteUrl_;
    QLineEdit*      remoteKey_;
    QCheckBox*      showKey_;
    QPushButton*    clearKey_;
    QLabel*         keyStore_;     // où la clé est conservée (trousseau, ou en clair)
    QLabel*         httpWarning_;  // clé envoyée en clair (http:// vers une autre machine)
    QComboBox*      remoteModel_;
    QPushButton*    listBtn_;
    QCheckBox*      remoteTools_;
    QLabel*         remoteStatus_;
    QLabel*         remoteWarning_;
    QWidget*        remoteKeyRow_;
    QWidget*        remoteModelRow_;
    QNetworkAccessManager* nam_;
    QHash<QString, remote::ModelLimits> remoteLimits_;   // relevées par « Lister les modèles »
    bool            optimizeAfterList_ = false;
    ModelInfo       localInfo_;
    double          localTps_ = 0;
    QPushButton*    optimizeBtn_;
    QLabel*         optimizeStatus_;
    void applyOptimization();
    // commun
    QDoubleSpinBox* temp_;
    QSpinBox*       maxTokens_;
    QCheckBox*      thinking_;
    QCheckBox*      keepHistory_;
    QSpinBox*       skillChars_;
    QLineEdit*      skillsDir_;
    QPlainTextEdit* userPrompt_;
    QComboBox*      dateFormat_;
    QSpinBox*       decimals_;
};

} // namespace seldon
