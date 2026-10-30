# Tests unitaires : qmake tests/tests.pro && make && ./tst_core
QT += core gui widgets network svg testlib
CONFIG += c++17 console testcase
TARGET = tst_core
INCLUDEPATH += ..
SOURCES += tst_core.cpp ../src/core/ChrBuilder.cpp ../src/core/Calculator.cpp ../src/core/ChronoResults.cpp ../src/core/ConversationStore.cpp ../src/ui/HistoryPanel.cpp ../src/skills/SkillManager.cpp ../src/skills/Lexicon.cpp ../src/ui/FigurePanel.cpp ../src/ui/ChrScene.cpp ../src/ui/HelpView.cpp ../src/core/MathText.cpp ../src/core/SecretStore.cpp ../src/core/AutoTune.cpp ../src/llm/RemoteApi.cpp ../src/llm/RemoteEngine.cpp ../src/llm/ToolsProbe.cpp ../src/ui/SettingsDialog.cpp ../src/ui/ChatView.cpp ../mcp/McpServer.cpp
HEADERS += ../src/core/ChronoResults.h ../src/core/ConversationStore.h ../src/ui/HistoryPanel.h ../src/core/ChatText.h ../src/core/ChrBuilder.h ../src/core/Calculator.h ../src/skills/SkillManager.h ../src/skills/Lexicon.h ../src/ui/FigurePanel.h ../src/ui/ChrScene.h ../src/ui/HelpView.h ../src/core/MathText.h ../src/core/SecretStore.h ../src/core/AutoTune.h ../src/llm/RemoteApi.h ../src/llm/RemoteEngine.h ../src/llm/ToolsProbe.h ../src/ui/SettingsDialog.h ../src/ui/ChatView.h ../mcp/McpServer.h
INCLUDEPATH += ../src/methods/common
RESOURCES += ../resources/resources.qrc
include(../src/methods/methods.pri)
