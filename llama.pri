# Liaison avec llama.cpp, compilé par :
#   macOS   : scripts/build_llama.sh        -> bibliothèques statiques, Metal, universel
#   Linux   : scripts/build_llama.sh        -> bibliothèques partagées + backends chargés au démarrage
#   Windows : scripts\build_llama_win.bat   -> idem (DLL), MinGW du kit Qt
# Résultat dans third_party/llama-install/{include,lib[,bin]}. Sans cette étape le projet ne se lie pas.

LLAMA_PREFIX = $$PWD/third_party/llama-install
INCLUDEPATH += $$LLAMA_PREFIX/include
LIBS += -L$$LLAMA_PREFIX/lib

LLAMA_DL = false
exists($$LLAMA_PREFIX/lib/libllama.so)|exists($$LLAMA_PREFIX/lib/libllama.dll.a): LLAMA_DL = true

equals(LLAMA_DL, true) {
    # Backends dynamiques (GGML_BACKEND_DL) : on ne lie que llama, ggml et ggml-base. Les backends
    # (ggml-cpu-<variante>, ggml-vulkan) sont chargés par LlmCore au démarrage, depuis le dossier de
    # l'application, sinon depuis SELDON_LLAMA_DIR (développement, lancement depuis Qt Creator).
    DEFINES += SELDON_GGML_DL
    LIBS += -lllama -lggml -lggml-base
    win32 {
        DEFINES += SELDON_LLAMA_DIR=\\\"$$LLAMA_PREFIX/bin\\\"
        # Développement : llama.dll, ggml*.dll doivent être à côté de l'exécutable pour qu'il démarre.
        LLAMA_DLL_DEST = $$OUT_PWD
        debug_and_release {
            CONFIG(debug, debug|release): LLAMA_DLL_DEST = $$OUT_PWD/debug
            else: LLAMA_DLL_DEST = $$OUT_PWD/release
        }
        QMAKE_POST_LINK += $$QMAKE_COPY $$shell_quote($$shell_path($$LLAMA_PREFIX/bin/*.dll)) $$shell_quote($$shell_path($$LLAMA_DLL_DEST)) $$escape_expand(\\n\\t)
    } else {
        DEFINES += SELDON_LLAMA_DIR=\\\"$$LLAMA_PREFIX/lib\\\"
        QMAKE_RPATHDIR += $$LLAMA_PREFIX/lib      # développement ; remplacé par $ORIGIN/lib au déploiement
    }
} else {
    !exists($$LLAMA_PREFIX/lib/libllama.a) {
        warning("llama.cpp introuvable dans $$LLAMA_PREFIX : exécutez scripts/build_llama.sh (ou scripts\\build_llama_win.bat)")
    }
    # Bibliothèques statiques (macOS). Ordre important : du plus haut niveau au plus bas.
    LIBS += -lllama -lggml
    macx {
        exists($$LLAMA_PREFIX/lib/libggml-metal.a) {
            LIBS += -lggml-metal
            LIBS += -framework Metal -framework MetalKit
        }
        LIBS += -lggml-blas -lggml-cpu -lggml-base
        LIBS += -framework Accelerate -framework Foundation
    } else {
        LIBS += -lggml-cpu -lggml-base
        unix: LIBS += -lgomp -ldl -lpthread
    }
}
