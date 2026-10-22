# Tests unitaires : qmake tests/tests.pro && make && ./tst_core
QT += core gui widgets network testlib
CONFIG += c++17 console testcase
TARGET = tst_core
INCLUDEPATH += ..
SOURCES += tst_core.cpp ../src/core/ChrBuilder.cpp ../src/core/ConversationStore.cpp ../src/ui/HistoryPanel.cpp ../src/skills/SkillManager.cpp ../src/ui/FigurePanel.cpp ../src/ui/HelpView.cpp ../src/core/MathText.cpp ../src/llm/RemoteApi.cpp ../src/llm/RemoteEngine.cpp ../src/ui/SettingsDialog.cpp ../src/ui/ChatView.cpp
HEADERS += ../src/core/ConversationStore.h ../src/ui/HistoryPanel.h ../src/core/ChatText.h ../src/core/ChrBuilder.h ../src/skills/SkillManager.h ../src/ui/FigurePanel.h ../src/ui/HelpView.h ../src/core/MathText.h ../src/llm/RemoteApi.h ../src/llm/RemoteEngine.h ../src/ui/SettingsDialog.h ../src/ui/ChatView.h
RESOURCES += ../resources/resources.qrc
include(../src/methods/methods.pri)
