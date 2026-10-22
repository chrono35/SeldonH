#!/usr/bin/env bash
# Compile llama.cpp et l'installe dans third_party/llama-install (lu par llama.pri).
#
# macOS : bibliothèques statiques, binaire universel arm64 + x86_64, Metal.
#   ./scripts/build_llama.sh                  (Metal activé sauf si la machine de compilation est un Mac Intel)
#   METAL=ON ./scripts/build_llama.sh         (version à distribuer : fait par QtIFW_script_macOS.sh)
#   ARCHS="arm64" ./scripts/build_llama.sh    (Apple Silicon seulement : plus rapide à compiler)
#   Metal sur Mac Intel : les GPU AMD/Intel anciens donnent un texte incohérent et ~40 s de compilation de shaders
#   au démarrage ; SeldonH reste donc sur le CPU par défaut sur ces Mac (AppSettings).
#
# Linux : bibliothèques partagées avec backends chargés au démarrage (GGML_BACKEND_DL), comme les versions
#   officielles de llama.cpp. Un seul exécutable pour tous les PC :
#     - CPU : une variante par jeu d'instructions (x64, sse42, sandybridge, haswell, skylakex, zen4…),
#             la meilleure est choisie au lancement ;
#     - GPU : backend Vulkan (NVIDIA, AMD, Intel), ignoré si le poste n'a pas de pilote Vulkan.
#   Paquets :  sudo apt install build-essential cmake git libvulkan-dev glslc spirv-headers
#   VULKAN=OFF ./scripts/build_llama.sh       (sans GPU, si le SDK Vulkan n'est pas disponible)
#
# Windows : voir scripts\build_llama_win.bat (même principe que Linux).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/llama.cpp"
PREFIX="$ROOT/third_party/llama-install"

# Version de llama.cpp contre laquelle le moteur a été écrit et vérifié (API C de llama.h).
LLAMA_COMMIT="${LLAMA_COMMIT:-f849095}"

if [ ! -d "$SRC/.git" ]; then
  git clone https://github.com/ggml-org/llama.cpp "$SRC"
fi
git -C "$SRC" fetch --quiet origin || true
git -C "$SRC" checkout --quiet "$LLAMA_COMMIT"

EXTRA=()
if [[ "$(uname)" == "Darwin" ]]; then
  BUILD="$ROOT/third_party/llama-build"
  METAL="${METAL:-}"
  if [[ -z "$METAL" ]]; then
    if [[ "$(uname -m)" != "x86_64" ]]; then METAL=ON; else METAL=OFF; fi
  fi
  SHARED=OFF
  EXTRA+=(-DGGML_METAL=$METAL -DGGML_METAL_EMBED_LIBRARY=$METAL -DGGML_ACCELERATE=ON -DGGML_OPENMP=OFF
          -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
          -DCMAKE_OSX_ARCHITECTURES="${ARCHS:-arm64;x86_64}")
  VARIANT="metal-$METAL"
else
  BUILD="$ROOT/third_party/llama-build-linux"
  VULKAN="${VULKAN:-ON}"
  SHARED=ON
  EXTRA+=(-DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON -DGGML_OPENMP=ON -DGGML_VULKAN=$VULKAN -DGGML_CUDA=OFF)
  VARIANT="dl-vulkan-$VULKAN"
fi

cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=$SHARED \
  -DGGML_NATIVE=OFF \
  -DLLAMA_CURL=OFF \
  -DLLAMA_BUILD_COMMON=OFF \
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF \
  "${EXTRA[@]}"
# La cible llama dépend de ggml, qui dépend de tous les backends (y compris les variantes CPU en mode DL).
cmake --build "$BUILD" --target llama --config Release -j "$(getconf _NPROCESSORS_ONLN)"

rm -rf "$PREFIX"
mkdir -p "$PREFIX/include" "$PREFIX/lib"
cp "$SRC"/include/llama.h "$SRC"/include/llama-cpp.h "$PREFIX/include/"
cp "$SRC"/ggml/include/*.h "$PREFIX/include/"
if [[ "$SHARED" == ON ]]; then
  # libllama.so*, libggml.so*, libggml-base.so* (liens symboliques compris) et backends libggml-cpu-*.so, libggml-vulkan.so
  find "$BUILD/bin" -maxdepth 1 \( -name 'lib*.so' -o -name 'lib*.so.*' \) -exec cp -a {} "$PREFIX/lib/" \;
else
  find "$BUILD" -name 'lib*.a' -exec cp {} "$PREFIX/lib/" \;
fi
printf 'os=%s\nvariant=%s\ncommit=%s\n' "$(uname)" "$VARIANT" "$LLAMA_COMMIT" > "$PREFIX/BUILD_INFO.txt"

echo "OK : llama.cpp installé dans $PREFIX ($VARIANT)"
ls "$PREFIX/lib"
if [[ "$(uname)" == "Darwin" ]]; then
  echo "Architectures de libllama.a :"; lipo -info "$PREFIX/lib/libllama.a"
fi
