#!/usr/bin/env bash
# Compile QtKeychain (clé API rangée dans le trousseau du système) et l'installe dans
# third_party/qtkeychain-install (lu par qtkeychain.pri).
#
# macOS : bibliothèque statique, binaire universel arm64 + x86_64 (Trousseau macOS, Security.framework) :
#         rien à livrer à côté de l'exécutable.
#   QTDIR=~/Qt/6.11.1/macos ./scripts/build_qtkeychain.sh
#   ARCHS="arm64" QTDIR=… ./scripts/build_qtkeychain.sh   (Apple Silicon seulement ; le kit Qt doit l'être aussi)
#
# Linux : bibliothèque partagée libqt6keychain.so, livrée dans lib/ par QtIFW_script_deb.sh.
#   Trousseau : Secret Service (GNOME Keyring, KWallet, KeePassXC…). libsecret est chargée au démarrage si elle
#   est présente (paquet libsecret-1-0, installé sur les bureaux courants) ; sinon QtKeychain parle directement
#   à KWallet / GNOME Keyring par D-Bus. Sans aucun trousseau, SeldonH garde la clé en clair et le signale.
#   Paquets :  sudo apt install build-essential cmake git pkg-config libsecret-1-dev
#   QTDIR=~/Qt/6.11.2/gcc_64 ./scripts/build_qtkeychain.sh
#
# Windows : voir scripts\build_qtkeychain_win.bat.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/qtkeychain"
PREFIX="$ROOT/third_party/qtkeychain-install"

# Version de QtKeychain (licence BSD 2 clauses) utilisée par SecretStore.
QTKEYCHAIN_TAG="${QTKEYCHAIN_TAG:-0.17.0}"

: "${QTDIR:?QTDIR doit indiquer le kit Qt 6 (ex. ~/Qt/6.11.2/gcc_64 ou ~/Qt/6.11.1/macos)}"
[ -f "$QTDIR/lib/cmake/Qt6/Qt6Config.cmake" ] || { echo "❌ Qt 6 introuvable dans $QTDIR (variable QTDIR)"; exit 1; }

if [ ! -d "$SRC/.git" ]; then
  git clone https://github.com/frankosterfeld/qtkeychain "$SRC"
fi
git -C "$SRC" fetch --quiet --tags origin || true
git -C "$SRC" checkout --quiet "$QTKEYCHAIN_TAG"

EXTRA=()
if [[ "$(uname)" == "Darwin" ]]; then
  BUILD="$ROOT/third_party/qtkeychain-build"
  SHARED=OFF
  EXTRA+=(-DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
          -DCMAKE_OSX_ARCHITECTURES="${ARCHS:-arm64;x86_64}")
  VARIANT="static-apple"
else
  BUILD="$ROOT/third_party/qtkeychain-build-linux"
  SHARED=ON
  pkg-config --exists libsecret-1 2>/dev/null \
    || { echo "❌ libsecret-1 introuvable : sudo apt install pkg-config libsecret-1-dev"; exit 1; }
  EXTRA+=(-DLIBSECRET_SUPPORT=ON)
  VARIANT="shared-libsecret"
fi

cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$QTDIR" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_WITH_QT5=OFF \
  -DBUILD_SHARED_LIBS=$SHARED \
  -DBUILD_TRANSLATIONS=OFF \
  -DBUILD_TEST_APPLICATION=OFF \
  -DBUILD_TESTING=OFF \
  "${EXTRA[@]}"
cmake --build "$BUILD" --config Release -j "$(getconf _NPROCESSORS_ONLN)"

rm -rf "$PREFIX"
cmake --install "$BUILD" --config Release
printf 'os=%s\nvariant=%s\ntag=%s\nqt=%s\n' "$(uname)" "$VARIANT" "$QTKEYCHAIN_TAG" "$QTDIR" > "$PREFIX/BUILD_INFO.txt"

echo "OK : QtKeychain installé dans $PREFIX ($VARIANT)"
ls "$PREFIX/include/qt6keychain" "$PREFIX/lib"
if [[ "$(uname)" == "Darwin" ]]; then
  echo "Architectures de libqt6keychain.a :"; lipo -info "$PREFIX/lib/libqt6keychain.a"
fi
