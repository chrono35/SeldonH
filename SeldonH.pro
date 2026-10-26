QT += widgets network svg

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000

TARGET = SeldonH

# Dossier du projet (pour trouver le dossier "models" en développement)
DEFINES += SELDON_PROJECT_DIR=\\\"$$PWD\\\"

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    src/core/ChrBuilder.cpp \
    src/core/ChronoResults.cpp \
    src/core/ConversationStore.cpp \
    src/llm/LlmCore.cpp \
    src/llm/LlmEngine.cpp \
    src/llm/RemoteApi.cpp \
    src/llm/RemoteEngine.cpp \
    src/skills/SkillManager.cpp \
    src/ui/ChatView.cpp \
    src/ui/FigurePanel.cpp \
    src/ui/HelpView.cpp \
    src/ui/HistoryPanel.cpp \
    src/core/MathText.cpp \
    src/core/SecretStore.cpp \
    src/ui/SettingsDialog.cpp

HEADERS += \
    mainwindow.h \
    src/core/AppSettings.h \
    src/core/ChatText.h \
    src/core/ChrBuilder.h \
    src/core/ChronoResults.h \
    src/core/ConversationStore.h \
    src/llm/LlmCore.h \
    src/llm/LlmEngine.h \
    src/llm/RemoteApi.h \
    src/llm/RemoteEngine.h \
    src/skills/SkillManager.h \
    src/ui/ChatView.h \
    src/ui/FigurePanel.h \
    src/ui/HelpView.h \
    src/ui/HistoryPanel.h \
    src/core/MathText.h \
    src/core/SecretStore.h \
    src/ui/InputEdit.h \
    src/ui/SettingsDialog.h

RESOURCES += resources/resources.qrc

# Icône de l'application (resources/icons/seldonh.svg est la source)
macx: ICON = $$PWD/resources/icons/SeldonH.icns
win32: RC_ICONS = $$PWD/resources/icons/SeldonH.ico

# Méthodes de datation portées de ChronoModel (c14, f14c, gauss, unif, tl, am) et leurs courbes
include(src/methods/methods.pri)

TRANSLATIONS += \
    SeldonH_fr_FR.ts
CONFIG += lrelease
CONFIG += embed_translations

# Moteur d'inférence llama.cpp (voir scripts/build_llama.sh)
include(llama.pri)

# Trousseau du système pour la clé API (voir scripts/build_qtkeychain.sh)
include(qtkeychain.pri)

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
