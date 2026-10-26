#!/bin/bash
# ===========================================================================
#  SeldonH - QtIFW_script_deb.sh   (Linux Debian / Ubuntu, x86_64)
#  Compile SeldonH (Release), rassemble les bibliothèques et plugins Qt, embarque
#  le modèle .gguf et construit l'installeur Qt Installer Framework (.run).
#
#  Usage :
#     bash QtInstaller_SeldonH/QtIFW_script_deb.sh                  installeur complet
#     bash QtInstaller_SeldonH/QtIFW_script_deb.sh rebuild          force la recompilation de llama.cpp et de QtKeychain
#     bash QtInstaller_SeldonH/QtIFW_script_deb.sh nomodel          installeur sans modèle (~100 Mo)
#     MODEL=Qwen3.5-4B-Q3_K_M.gguf bash QtInstaller_SeldonH/QtIFW_script_deb.sh   autre modèle
#
#  Un seul installeur pour tous les PC : llama.cpp est livré avec ses backends chargés au démarrage
#  (CPU : meilleure variante du processeur ; GPU : Vulkan si le pilote le permet, sinon ignoré).
#  Rien à compiler pour l'utilisateur.
#  Clé API : QtKeychain (libqt6keychain.so, livrée dans lib/) la range dans le trousseau du bureau
#  (Secret Service : GNOME Keyring, KWallet…). libsecret n'est pas livrée : c'est celle du système, chargée si présente.
#
#  Paquets pour compiler :  sudo apt install build-essential cmake git patchelf libgl1-mesa-dev libxkbcommon-dev \
#                                            libvulkan-dev glslc spirv-headers pkg-config libsecret-1-dev
#  Sur le poste cible (Qt 6 / xcb) : sudo apt install libxcb-cursor0   (souvent déjà présent)
#  L'installeur fonctionne sur la version de Debian où il a été compilé et les suivantes (glibc) :
#  compiler sur la plus ancienne version visée.
#
#  Project ChronoModel Team - https://chronomodel.com/
#  author email: philippe.dufresne@univ-rennes.fr
#  date 2026-10-22
# ===========================================================================
# ne pas mettre de blanc autour de =
set -euo pipefail
clear

# ---------------------------- A VERIFIER ---------------------------------
VERSION=1.0.0
QTDIR=${QTDIR:-$HOME/Qt/6.11.2/gcc_64}
IFW=${IFW:-$HOME/Qt/Tools/QtInstallerFramework/4.10/bin}
MODEL=${MODEL-Qwen3.5-4B-Q4_K_M.gguf}      # vide = installeur sans modèle
COMPRESSION=${COMPRESSION:-0}              # 0 : un .gguf ne se compresse pas ; LZMA sur 2,7 Go = très long
# -------------------------------------------------------------------------

REBUILD=0
for a in "$@"; do
    case "$a" in
        rebuild) REBUILD=1 ;;
        nomodel) MODEL="" ;;
    esac
done

INST="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$INST/.." && pwd)"
PKGS="$INST/installer-packages-deb"
DATA="$PKGS/seldonh.app/data"
MODEL_DIR="$PKGS/seldonh.model/data/models"
BUILD_DIR="$ROOT/build/linux64-Release"
LLAMA="$ROOT/third_party/llama-install"
KEYCHAIN="$ROOT/third_party/qtkeychain-install"
NPROC=$(nproc)

echo "➡️  1 - Vérification des chemins"
[ -x "$QTDIR/bin/qmake" ]        || { echo "❌ Qt introuvable : $QTDIR (variable QTDIR)"; exit 1; }
[ -x "$IFW/binarycreator" ]      || { echo "❌ QtIFW introuvable : $IFW (variable IFW)"; exit 1; }
if [ -n "$MODEL" ] && [ ! -f "$ROOT/models/$MODEL" ]; then
    echo "❌ modèle introuvable : $ROOT/models/$MODEL (le télécharger, ou argument nomodel)"; exit 1
fi
command -v patchelf >/dev/null || echo "⚠️  patchelf absent (sudo apt install patchelf) : le lanceur SeldonH.sh restera indispensable"

QT_NUM=$(basename "$(dirname "$QTDIR")")          # ~/Qt/6.11.2/gcc_64 -> 6.11.2
QT_VER_STR="Qt${QT_NUM//./_}"
DATE_FILE=$(date '+%Y%m%d')
DATE_ISO=$(date '+%Y-%m-%d')

echo "➡️  2 - llama.cpp (backends dynamiques CPU + Vulkan)"
if [ "$REBUILD" = 1 ] || [ ! -e "$LLAMA/lib/libllama.so" ]; then
    echo "   compilation de llama.cpp"
    bash "$ROOT/scripts/build_llama.sh"
else
    cat "$LLAMA/BUILD_INFO.txt"
fi

echo "➡️  2b - QtKeychain (clé API dans le trousseau du système)"
if [ "$REBUILD" = 1 ] || [ ! -e "$KEYCHAIN/lib/libqt6keychain.so" ]; then
    echo "   compilation de QtKeychain"
    QTDIR="$QTDIR" bash "$ROOT/scripts/build_qtkeychain.sh"
else
    cat "$KEYCHAIN/BUILD_INFO.txt"
fi

echo "➡️  3 - Compilation de SeldonH (Release) dans $BUILD_DIR"
mkdir -p "$BUILD_DIR"
# qmake relancé à chaque fois : llama.pri choisit le mode de liaison de llama.cpp à cette étape (et qtkeychain.pri
# active le trousseau) ;
# l'exécutable est supprimé pour forcer l'édition de liens avec les bibliothèques llama.cpp actuelles.
rm -f "$BUILD_DIR/SeldonH"
( cd "$BUILD_DIR" && "$QTDIR/bin/qmake" "$ROOT/SeldonH.pro" "CONFIG+=release" && make -j"$NPROC" )
EXE="$BUILD_DIR/SeldonH"
[ -x "$EXE" ] || { echo "❌ exécutable introuvable : $EXE"; exit 1; }

echo "➡️  4 - Préparation du composant seldonh.app"
rm -rf "$DATA"
mkdir -p "$DATA/lib" "$DATA/plugins"
cp "$EXE" "$DATA/"
cp "$ROOT/resources/icons/seldonh_256.png" "$DATA/seldonh.png"
cp "$ROOT/third_party/llama.cpp/LICENSE" "$PKGS/seldonh.app/meta/llama.cpp-LICENSE.txt"
cp "$ROOT/third_party/qtkeychain/COPYING" "$PKGS/seldonh.app/meta/qtkeychain-LICENSE.txt"
cp "$ROOT/LICENSE" "$PKGS/seldonh.app/meta/SeldonH-LICENSE.txt"
cp "$ROOT/COPYING" "$PKGS/seldonh.app/meta/GPL-3.0.txt"
cp "$ROOT/LICENSE" "$ROOT/COPYING" "$DATA/"
# llama.cpp : libllama, libggml, libggml-base (+ liens symboliques) et backends libggml-cpu-*.so, libggml-vulkan.so
cp -a "$LLAMA"/lib/*.so* "$DATA/lib/"
ls "$DATA/lib" | grep -E '^libggml-(cpu|vulkan)' | sed 's/^/   backend : /'
# QtKeychain : libqt6keychain.so* (+ liens symboliques) ; sa dépendance Qt6DBus est ajoutée à l'étape 6
cp -a "$KEYCHAIN"/lib/libqt6keychain.so* "$DATA/lib/"

echo "➡️  5 - Plugins Qt"
# xcb seulement (fonctionne aussi sous Wayland via XWayland) ; compose = touches mortes (accents ^ ¨ …)
for p in platforms/libqxcb.so xcbglintegrations imageformats iconengines tls networkinformation \
         platforminputcontexts/libcomposeplatforminputcontextplugin.so \
         platforminputcontexts/libibusplatforminputcontextplugin.so \
         platformthemes/libqxdgdesktopportal.so; do
    src="$QTDIR/plugins/$p"
    [ -e "$src" ] || { echo "   (absent : $p)"; continue; }
    mkdir -p "$DATA/plugins/$(dirname "$p")"
    cp -R "$src" "$DATA/plugins/$(dirname "$p")/"
done
find "$DATA/plugins" -name '*.debug' -delete

echo "➡️  6 - Bibliothèques (Qt, ICU, OpenMP)"
# ldd sur l'exécutable, les plugins et les bibliothèques llama.cpp : on garde ce qui vient du kit Qt (dont ICU)
# et libgomp (pas toujours installé). libvulkan.so.1 reste celle du système (pilote) : si elle manque,
# le backend Vulkan est simplement ignoré. libsecret-1.so.0 (trousseau) reste aussi celle du système.
collect() {
    LD_LIBRARY_PATH="$DATA/lib:$QTDIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$1" | awk '/=> \//{print $3}' | while read -r lib; do
        keep=0
        case "$lib" in
            "$QTDIR"/lib/*|*/libgomp.so*) keep=1 ;;
        esac
        if [ "$keep" = 1 ] && [ ! -e "$DATA/lib/$(basename "$lib")" ]; then
            cp -L "$lib" "$DATA/lib/"
        fi
    done
    if LD_LIBRARY_PATH="$DATA/lib:$QTDIR/lib" ldd "$1" | grep -q "not found"; then
        echo "⚠️  dépendance non trouvée pour $1 :"; LD_LIBRARY_PATH="$DATA/lib:$QTDIR/lib" ldd "$1" | grep "not found" || true
    fi
}
collect "$DATA/SeldonH"
while IFS= read -r -d '' so; do collect "$so"; done < <(find "$DATA/plugins" "$DATA/lib" -name 'lib*.so*' -type f -print0)

cat > "$DATA/qt.conf" <<'QTCONF'
[Paths]
Prefix = .
Libraries = lib
Plugins = plugins
QTCONF

cat > "$DATA/SeldonH.sh" <<'LAUNCH'
#!/bin/sh
# Lanceur SeldonH : bibliothèques embarquées dans ./lib, plateforme xcb (X11 ou XWayland)
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
exec "$HERE/SeldonH" "$@"
LAUNCH
chmod +x "$DATA/SeldonH.sh" "$DATA/SeldonH"

if command -v patchelf >/dev/null; then
    patchelf --set-rpath '$ORIGIN/lib' "$DATA/SeldonH"
    find "$DATA/lib" -name '*.so*' -type f -exec patchelf --set-rpath '$ORIGIN' {} \;
fi
echo "   $(ls "$DATA/lib" | wc -l) bibliothèques, $(du -sh "$DATA" | cut -f1) au total"

echo "➡️  7 - Composant seldonh.model"
EXCLUDE=()
SUFFIX=""
if [ -z "$MODEL" ]; then
    echo "   pas de modèle embarqué"
    EXCLUDE=(--exclude seldonh.model)
    SUFFIX="_sansModele"
else
    mkdir -p "$MODEL_DIR"
    find "$MODEL_DIR" -name '*.gguf' ! -name "$MODEL" -delete   # un seul .gguf (SeldonH charge le premier)
    if [ "$(stat -c %s "$ROOT/models/$MODEL")" != "$(stat -c %s "$MODEL_DIR/$MODEL" 2>/dev/null || echo -1)" ]; then
        echo "   copie de $MODEL …"
        cp "$ROOT/models/$MODEL" "$MODEL_DIR/"
    else
        echo "   $MODEL déjà en place"
    fi
fi

echo "➡️  8 - Version $VERSION et date $DATE_ISO dans config.xml / package.xml"
sed -i -E "s|<Version>[^<]*</Version>|<Version>$VERSION</Version>|; s|<ReleaseDate>[^<]*</ReleaseDate>|<ReleaseDate>$DATE_ISO</ReleaseDate>|" \
    "$INST/installer-config/config.xml" "$PKGS/seldonh.app/meta/package.xml" "$PKGS/seldonh.model/meta/package.xml"

INSTALLER="SeldonH_v${VERSION}_${QT_VER_STR}_linux64_${DATE_FILE}_Installer${SUFFIX}.run"
echo "➡️  9 - binarycreator : $INSTALLER"
AC=()
"$IFW/binarycreator" --help 2>&1 | grep -q -- '--ac' && AC=(--ac "$COMPRESSION")
cd "$INST"
"$IFW/binarycreator" --offline-only "${AC[@]}" "${EXCLUDE[@]}" -c installer-config/config.xml -p installer-packages-deb "$INSTALLER"
chmod +x "$INSTALLER"

echo "✅ 10 - Terminé : $INST/$INSTALLER"
