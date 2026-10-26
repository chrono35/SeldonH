#pragma once

#include <QDialog>

#include "../core/AppSettings.h"

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

public slots:
    void done(int r) override;     // OK : confirmation si la clé API partirait en clair (http://)

private slots:
    void browseModel();
    void browseSkills();
    void updateBackendRows();
    void listRemoteModels();
    void updateHttpWarning();

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
    // commun
    QDoubleSpinBox* temp_;
    QSpinBox*       maxTokens_;
    QCheckBox*      thinking_;
    QCheckBox*      keepHistory_;
    QSpinBox*       skillChars_;
    QLineEdit*      skillsDir_;
    QPlainTextEdit* userPrompt_;
    QComboBox*      dateFormat_;
};

} // namespace seldon
