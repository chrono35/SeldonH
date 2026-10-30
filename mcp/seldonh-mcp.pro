# seldonh-mcp : serveur MCP (stdio) des outils de calcul de SeldonH, pour Claude Desktop, Jan, Gemini CLI…
#   qmake mcp/seldonh-mcp.pro && make        (QtCore seulement : ni interface, ni réseau, ni llama.cpp)
QT = core
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = seldonh-mcp

VERSION = 1.0.0
DEFINES += APP_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD $$PWD/../src/methods/common
SOURCES += $$PWD/main.cpp $$PWD/McpServer.cpp
HEADERS += $$PWD/McpServer.h

# Méthodes de datation et courbes de référence (les mêmes que l'application)
include(../src/methods/methods.pri)

unix:!macx: QMAKE_LFLAGS += -pthread
# macOS : installé dans SeldonH.app/Contents/MacOS ; qmake met déjà @executable_path/../Frameworks dans le rpath
# (le répéter ici donne « duplicate -rpath … ignored ») et macdeployqt -executable=… y place les frameworks Qt.
