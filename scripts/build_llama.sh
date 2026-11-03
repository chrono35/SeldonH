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
DARWIN_ARCHS=()
NARCH=0          # nombre d'architectures macOS (0 ailleurs) ; bash 3.2 de macOS : pas de "${tableau_vide[@]}" avec set -u
if [[ "$(uname)" == "Darwin" ]]; then
  BUILD="$ROOT/third_party/llama-build"
  METAL="${METAL:-}"
  if [[ -z "$METAL" ]]; then
    if [[ "$(uname -m)" != "x86_64" ]]; then METAL=ON; else METAL=OFF; fi
  fi
  SHARED=OFF
  EXTRA+=(-DGGML_METAL=$METAL -DGGML_METAL_EMBED_LIBRARY=$METAL -DGGML_ACCELERATE=ON -DGGML_OPENMP=OFF
          -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}")
  # Une compilation PAR architecture, puis lipo. Avec CMAKE_OSX_ARCHITECTURES="arm64;x86_64" en un seul passage,
  # ggml ne reconnaît pas l'architecture et compile le CPU en -DGGML_CPU_GENERIC : ni NEON optimisé (arm64)
  # ni AVX2/FMA/F16C (x86_64), soit une génération CPU plusieurs fois plus lente (Mac Intel surtout).
  # x86_64 : AVX2 + FMA + F16C (Haswell, 2013, et suivants : tous les Mac Intel sous macOS 12 sauf le Mac Pro 2013).
  IFS=';' read -r -a DARWIN_ARCHS <<< "${ARCHS:-arm64;x86_64}"
  NARCH=${#DARWIN_ARCHS[@]}
  VARIANT="metal-$METAL"
else
  BUILD="$ROOT/third_party/llama-build-linux"
  VULKAN="${VULKAN:-ON}"
  SHARED=ON
  EXTRA+=(-DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON -DGGML_OPENMP=ON -DGGML_VULKAN=$VULKAN -DGGML_CUDA=OFF)
  VARIANT="dl-vulkan-$VULKAN"
fi

configure_and_build() {   # $1 = dossier de compilation, puis options cmake supplémentaires
  local dir="$1"; shift
  cmake -S "$SRC" -B "$dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=$SHARED \
    -DGGML_NATIVE=OFF \
    -DLLAMA_CURL=OFF \
    -DLLAMA_BUILD_COMMON=OFF \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF \
    "${EXTRA[@]}" "$@"
  # La cible llama dépend de ggml, qui dépend de tous les backends (y compris les variantes CPU en mode DL).
  cmake --build "$dir" --target llama --config Release -j "$(getconf _NPROCESSORS_ONLN)"
}

if [[ $NARCH -gt 0 ]]; then
  for arch in "${DARWIN_ARCHS[@]}"; do
    archopts=(-DCMAKE_OSX_ARCHITECTURES="$arch")
    [[ "$arch" == x86_64 ]] && archopts+=(-DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_AVX512=OFF)
    # Ancien dossier universel (un seul passage) : le cache CMake garderait les deux architectures.
    if [[ -f "$BUILD-$arch/CMakeCache.txt" ]] && ! grep -q "CMAKE_OSX_ARCHITECTURES:STRING=$arch\$" "$BUILD-$arch/CMakeCache.txt"; then
      rm -rf "$BUILD-$arch"
    fi
    configure_and_build "$BUILD-$arch" "${archopts[@]}"
    if grep -rqs -- "-DGGML_CPU_GENERIC" "$BUILD-$arch/ggml/src" --include=flags.make; then
      echo "ERREUR : ggml-cpu compilé en GGML_CPU_GENERIC pour $arch (aucune optimisation SIMD)." >&2; exit 1
    fi
  done
else
  configure_and_build "$BUILD"
fi

rm -rf "$PREFIX"
mkdir -p "$PREFIX/include" "$PREFIX/lib"
cp "$SRC"/include/llama.h "$SRC"/include/llama-cpp.h "$PREFIX/include/"
cp "$SRC"/ggml/include/*.h "$PREFIX/include/"
if [[ "$SHARED" == ON ]]; then
  # libllama.so*, libggml.so*, libggml-base.so* (liens symboliques compris) et backends libggml-cpu-*.so, libggml-vulkan.so
  find "$BUILD/bin" -maxdepth 1 \( -name 'lib*.so' -o -name 'lib*.so.*' \) -exec cp -a {} "$PREFIX/lib/" \;
elif [[ $NARCH -gt 1 ]]; then
  # Bibliothèques universelles : lipo des bibliothèques de même nom de chaque architecture.
  first="$BUILD-${DARWIN_ARCHS[0]}"
  while IFS= read -r lib; do
    name="$(basename "$lib")"
    parts=()
    for arch in "${DARWIN_ARCHS[@]}"; do
      other="$(find "$BUILD-$arch" -name "$name" | head -n 1)"
      [[ -n "$other" ]] || { echo "ERREUR : $name absent pour $arch" >&2; exit 1; }
      parts+=("$other")
    done
    lipo -create "${parts[@]}" -output "$PREFIX/lib/$name"
  done < <(find "$first" -name 'lib*.a')
else
  find "$BUILD-${DARWIN_ARCHS[0]}" -name 'lib*.a' -exec cp {} "$PREFIX/lib/" \;
fi
printf 'os=%s\nvariant=%s\ncommit=%s\n' "$(uname)" "$VARIANT" "$LLAMA_COMMIT" > "$PREFIX/BUILD_INFO.txt"

echo "OK : llama.cpp installé dans $PREFIX ($VARIANT)"
ls "$PREFIX/lib"
if [[ "$(uname)" == "Darwin" ]]; then
  echo "Architectures de libllama.a :"; lipo -info "$PREFIX/lib/libllama.a"
fi
