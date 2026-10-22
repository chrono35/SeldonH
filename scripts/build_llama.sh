#!/usr/bin/env bash
# Compile llama.cpp en bibliothèques statiques et les installe dans third_party/llama-install.
# Usage :  ./scripts/build_llama.sh                  (macOS : binaire universel arm64 + x86_64, Metal activé)
#          ARCHS="arm64" ./scripts/build_llama.sh    (Apple Silicon seulement : plus rapide à compiler)
# Les bibliothèques doivent avoir les mêmes architectures que le kit Qt utilisé par Qt Creator.
#
# Metal (GPU) : activé par défaut, sauf sur un Mac Intel (uname -m = x86_64) où les GPU AMD/Intel anciens
# ne gèrent pas les noyaux des modèles récents (texte incohérent) et où Metal ajoute ~40 s de compilation
# de shaders au démarrage. Forcer :  METAL=ON ./scripts/build_llama.sh   (ex. pour une version à distribuer)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/llama.cpp"
BUILD="$ROOT/third_party/llama-build"
PREFIX="$ROOT/third_party/llama-install"

# Version de llama.cpp contre laquelle le moteur a été écrit et vérifié (API C de llama.h).
LLAMA_COMMIT="${LLAMA_COMMIT:-f849095}"

if [ ! -d "$SRC/.git" ]; then
  git clone https://github.com/ggml-org/llama.cpp "$SRC"
fi
git -C "$SRC" fetch --quiet origin || true
git -C "$SRC" checkout --quiet "$LLAMA_COMMIT"

METAL="${METAL:-}"
if [[ -z "$METAL" ]]; then
  if [[ "$(uname)" == "Darwin" && "$(uname -m)" != "x86_64" ]]; then METAL=ON; else METAL=OFF; fi
fi

EXTRA=()
if [[ "$(uname)" == "Darwin" ]]; then
  EXTRA+=(-DGGML_METAL=$METAL -DGGML_METAL_EMBED_LIBRARY=$METAL -DGGML_ACCELERATE=ON -DGGML_OPENMP=OFF
          -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}")
  EXTRA+=(-DCMAKE_OSX_ARCHITECTURES="${ARCHS:-arm64;x86_64}")
fi

cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_NATIVE=OFF \
  -DLLAMA_CURL=OFF \
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF \
  "${EXTRA[@]}"
cmake --build "$BUILD" --target llama --config Release -j "$(getconf _NPROCESSORS_ONLN)"

rm -rf "$PREFIX"
mkdir -p "$PREFIX/include" "$PREFIX/lib"
cp "$SRC"/include/llama.h "$SRC"/include/llama-cpp.h "$PREFIX/include/"
cp "$SRC"/ggml/include/*.h "$PREFIX/include/"
find "$BUILD" -name 'lib*.a' -exec cp {} "$PREFIX/lib/" \;

echo "OK : llama.cpp installé dans $PREFIX (Metal : ${METAL})"
ls "$PREFIX/lib"
if [[ "$(uname)" == "Darwin" ]]; then
  echo "Architectures de libllama.a :"; lipo -info "$PREFIX/lib/libllama.a"
fi
