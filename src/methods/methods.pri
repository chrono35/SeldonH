# Méthodes de datation (une par dossier) : à inclure depuis SeldonH.pro et tests/tests.pro
#   include(src/methods/methods.pri)   (le chemin est relatif à la racine du projet)
METHODS_DIR = $$PWD

INCLUDEPATH += $$METHODS_DIR

SOURCES += \
    $$METHODS_DIR/common/Density.cpp \
    $$METHODS_DIR/common/DensityFile.cpp \
    $$METHODS_DIR/common/EventMcmc.cpp \
    $$METHODS_DIR/common/RefCurve.cpp \
    $$METHODS_DIR/common/CurveStore.cpp \
    $$METHODS_DIR/common/Method.cpp \
    $$METHODS_DIR/common/MethodRegistry.cpp \
    $$METHODS_DIR/common/MethodRunner.cpp \
    $$METHODS_DIR/common/Tools.cpp \
    $$METHODS_DIR/c14/C14Method.cpp \
    $$METHODS_DIR/f14c/F14cMethod.cpp \
    $$METHODS_DIR/gauss/GaussMethod.cpp \
    $$METHODS_DIR/unif/UnifMethod.cpp \
    $$METHODS_DIR/tl/TlMethod.cpp \
    $$METHODS_DIR/am/AmMethod.cpp

HEADERS += \
    $$METHODS_DIR/common/Density.h \
    $$METHODS_DIR/common/DensityFile.h \
    $$METHODS_DIR/common/EventMcmc.h \
    $$METHODS_DIR/common/RefCurve.h \
    $$METHODS_DIR/common/CurveStore.h \
    $$METHODS_DIR/common/Method.h \
    $$METHODS_DIR/common/MethodRegistry.h \
    $$METHODS_DIR/common/MethodRunner.h \
    $$METHODS_DIR/common/Tools.h \
    $$METHODS_DIR/common/TextUtil.h \
    $$METHODS_DIR/common/DateFormat.h \
    $$METHODS_DIR/c14/C14Method.h \
    $$METHODS_DIR/f14c/F14cMethod.h \
    $$METHODS_DIR/gauss/GaussMethod.h \
    $$METHODS_DIR/unif/UnifMethod.h \
    $$METHODS_DIR/tl/TlMethod.h \
    $$METHODS_DIR/am/AmMethod.h

# Courbes de référence de ChronoModel, un fichier .qrc par méthode
RESOURCES += \
    $$METHODS_DIR/c14/c14.qrc \
    $$METHODS_DIR/f14c/f14c.qrc \
    $$METHODS_DIR/am/am.qrc
