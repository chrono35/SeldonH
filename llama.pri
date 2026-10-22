# Liaison avec llama.cpp compilé en bibliothèques statiques par scripts/build_llama.sh
# (résultat dans third_party/llama-install/{include,lib}).
# Sans cette étape, le projet ne se lie pas : lancez d'abord  ./scripts/build_llama.sh

LLAMA_PREFIX = $$PWD/third_party/llama-install
!exists($$LLAMA_PREFIX/lib/libllama.a) {
    warning("llama.cpp introuvable dans $$LLAMA_PREFIX : exécutez scripts/build_llama.sh")
}

INCLUDEPATH += $$LLAMA_PREFIX/include
LIBS += -L$$LLAMA_PREFIX/lib

# Ordre important pour les bibliothèques statiques : du plus haut niveau au plus bas.
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
