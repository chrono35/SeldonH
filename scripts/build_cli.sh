#!/usr/bin/env bash
# Compile l'outil de test en ligne de commande (sans Qt) : build/llm_cli
# Utile pour savoir si un problème vient du moteur/du modèle ou de l'interface.
#   ./scripts/build_cli.sh
#   ./build/llm_cli models/Qwen3.5-4B-Q4_K_M.gguf --cpu     # CPU seul
#   ./build/llm_cli models/Qwen3.5-4B-Q4_K_M.gguf           # avec GPU (Metal)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
P="$ROOT/third_party/llama-install"
mkdir -p "$ROOT/build"
if [[ "$(uname)" == "Darwin" ]]; then
  LIBS=(-lllama -lggml)
  if [[ -f "$P/lib/libggml-metal.a" ]]; then LIBS+=(-lggml-metal -framework Metal -framework MetalKit); fi
  LIBS+=(-lggml-blas -lggml-cpu -lggml-base -framework Accelerate -framework Foundation)
elif [[ -e "$P/lib/libllama.so" ]]; then
  # Linux, backends dynamiques (scripts/build_llama.sh) : chargés depuis $P/lib au démarrage
  LIBS=(-DSELDON_GGML_DL "-DSELDON_LLAMA_DIR=\"$P/lib\"" -lllama -lggml -lggml-base "-Wl,-rpath,$P/lib")
else
  LIBS=(-lllama -lggml -lggml-cpu -lggml-base -lgomp -ldl -lpthread)
fi
c++ -std=c++17 -O2 -I "$P/include" "$ROOT/tests/llm_cli.cpp" "$ROOT/src/llm/LlmCore.cpp" \
    -L "$P/lib" "${LIBS[@]}" -o "$ROOT/build/llm_cli"
echo "OK : $ROOT/build/llm_cli"
