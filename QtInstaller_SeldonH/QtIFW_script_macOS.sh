#!/bin/bash
# ===========================================================================
#  SeldonH - QtIFW_script_macOS.sh   (macOS, binaire universel x86_64 + arm64)
#  Compile SeldonH et le serveur MCP seldonh-mcp (Release universel), copie les bibliothèques Qt (macdeployqt),
#  signe le bundle (ad hoc), embarque le modèle .gguf, construit l'installeur
#  Qt Installer Framework et le met dans un .dmg avec un LISEZMOI.
#  Reprend Ch_copy_library_and_signature.sh + QtIFW_script_macOS.sh + Ch_make_dmg_macOS.sh de ChronoModel.
#
#  A lancer avec bash (pas sh) :
#     bash QtInstaller_SeldonH/QtIFW_script_macOS.sh             installeur complet + .dmg
#     bash QtInstaller_SeldonH/QtIFW_script_macOS.sh rebuild     force la recompilation de llama.cpp et de QtKeychain
#     bash QtInstaller_SeldonH/QtIFW_script_macOS.sh noModel     installeur sans modèle
#     bash QtInstaller_SeldonH/QtIFW_script_macOS.sh nodmg       pas de .dmg (installeur .app seul)
#     MODEL=Qwen3.5-4B-Q3_K_M.gguf bash QtInstaller_SeldonH/QtIFW_script_macOS.sh   autre modèle
#
#  Un seul installeur pour tous les Mac : binaire universel avec Metal (GPU) et CPU. Sur Apple Silicon,
#  SeldonH utilise le GPU ; sur un Mac Intel il reste par défaut sur le processeur (AppSettings :
#  nGpuLayers = 0), Metal n'y étant pas fiable. Rien à compiler pour l'utilisateur.
#  Clé API : QtKeychain, lié en statique (Trousseau macOS) : rien à ajouter au bundle.
#
#  Ordre (une modification du bundle après la signature invalide celle-ci) :
#     copie du bundle -> macdeployqt -> Info.plist -> signature de l'intérieur vers l'extérieur -> vérifications
#  Le modèle est installé HORS du bundle (/Applications/SeldonH/models) pour la même raison.
#
#  Project ChronoModel Team - https://chronomodel.com/
#  author email: philippe.dufresne@univ-rennes.fr
#  date 2026-10-22
# ===========================================================================
# Pas de blanc autour de = dans les affectations.
set -euo pipefail
clear || true

# ---------------------------- A VERIFIER ---------------------------------
VERSION=1.0.0
QT_VERSION=6.11.1
QT_ROOT=/Users/dufresne/Qt/$QT_VERSION/macos
IFW=/Users/dufresne/Qt/Tools/QtInstallerFramework/4.11/bin
MODEL=${MODEL-Qwen3.5-4B-Q4_K_M.gguf}      # vide = installeur sans modèle
COMPRESSION=${COMPRESSION:-0}              # 0 : un .gguf ne se compresse pas ; LZMA sur 2,7 Go = très long
ARCHS="x86_64 arm64"                       # binaire universel (Qt et llama.cpp doivent l'être aussi)

# Signature "ad hoc" (identité "-") : gratuite, sans compte Apple payant. Valide (obligatoire sur
# Apple Silicon) mais non reconnue par Gatekeeper : voir le LISEZMOI du .dmg.
SIGN_ID="-"
# -------------------------------------------------------------------------

REBUILD=0
MAKE_DMG=1
for a in "$@"; do
    case "$a" in
        rebuild) REBUILD=1 ;;
        nomodel|noModel|NOMODEL) MODEL="" ;;
        nodmg)   MAKE_DMG=0 ;;
    esac
done
METAL=ON

INST="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$INST/.." && pwd)"
PKGS="$INST/installer-packages-macOS"
DATA="$PKGS/seldonh_app/data"
MODEL_DIR="$PKGS/seldonh_model/data/models"
BUILD_DIR="$ROOT/build/macOS-Release"
LLAMA="$ROOT/third_party/llama-install"
KEYCHAIN="$ROOT/third_party/qtkeychain-install"
STAGING="$INST/staging"
BUNDLE="$STAGING/SeldonH.app"
EXE="$BUNDLE/Contents/MacOS/SeldonH"
MCP_BUILD_DIR="$ROOT/build/macOS-Release-mcp"
MCP_EXE="$BUNDLE/Contents/MacOS/seldonh-mcp"      # dans le bundle : mêmes frameworks Qt, même signature
NPROC=$(sysctl -n hw.ncpu)

QT_VER_STR="Qt${QT_VERSION//./_}"
DATE_FILE=$(date '+%Y%m%d')
DATE_ISO=$(date '+%Y-%m-%d')

is_universal() {   # $1 = fichier Mach-O ou bibliothèque statique
    local a; a=$(lipo -archs "$1" 2>/dev/null || true)
    [[ "$a" == *x86_64* && "$a" == *arm64* ]]
}

echo "➡️  1 - Vérification des chemins"
[ -x "$QT_ROOT/bin/qmake" ]        || { echo "❌ Qt introuvable : $QT_ROOT"; exit 1; }
[ -x "$QT_ROOT/bin/macdeployqt" ]  || { echo "❌ macdeployqt introuvable dans $QT_ROOT/bin"; exit 1; }
[ -x "$IFW/binarycreator" ]        || { echo "❌ QtIFW introuvable : $IFW"; exit 1; }
is_universal "$QT_ROOT/lib/QtCore.framework/QtCore" || { echo "❌ Le kit Qt n'est pas universel (x86_64 + arm64)"; exit 1; }
if [ -n "$MODEL" ] && [ ! -f "$ROOT/models/$MODEL" ]; then
    echo "❌ modèle introuvable : $ROOT/models/$MODEL (le télécharger, ou argument noModel)"; exit 1
fi

echo "➡️  2 - llama.cpp (Metal, universel)"
LLAMA_VARIANT=$(sed -n 's/^variant=//p' "$LLAMA/BUILD_INFO.txt" 2>/dev/null || true)
if [ "$REBUILD" = 1 ] || [ ! -f "$LLAMA/lib/libllama.a" ] || [ "$LLAMA_VARIANT" != "metal-$METAL" ] \
   || ! is_universal "$LLAMA/lib/libllama.a"; then
    echo "   compilation de llama.cpp (installé : '${LLAMA_VARIANT}', demandé : 'metal-$METAL')"
    METAL=$METAL ARCHS="arm64;x86_64" bash "$ROOT/scripts/build_llama.sh"
else
    echo "   déjà compilé (metal-$METAL, universel)"
fi

echo "➡️  2b - QtKeychain (Trousseau macOS, statique, universel)"
if [ "$REBUILD" = 1 ] || [ ! -f "$KEYCHAIN/lib/libqt6keychain.a" ] || ! is_universal "$KEYCHAIN/lib/libqt6keychain.a"; then
    echo "   compilation de QtKeychain"
    QTDIR="$QT_ROOT" ARCHS="arm64;x86_64" bash "$ROOT/scripts/build_qtkeychain.sh"
else
    echo "   déjà compilé (universel)"
fi

echo "➡️  3 - Compilation de SeldonH (Release universel) dans $BUILD_DIR"
mkdir -p "$BUILD_DIR"
# qmake relancé à chaque fois (llama.pri choisit Metal ou non) ; bundle supprimé pour forcer l'édition de liens.
rm -rf "$BUILD_DIR/SeldonH.app"
( cd "$BUILD_DIR" && "$QT_ROOT/bin/qmake" "$ROOT/SeldonH.pro" "CONFIG+=release" "QMAKE_APPLE_DEVICE_ARCHS=$ARCHS" \
  && make -j"$NPROC" )
[ -x "$BUILD_DIR/SeldonH.app/Contents/MacOS/SeldonH" ] || { echo "❌ bundle introuvable : $BUILD_DIR/SeldonH.app"; exit 1; }
echo "   Architectures : $(lipo -archs "$BUILD_DIR/SeldonH.app/Contents/MacOS/SeldonH")"
is_universal "$BUILD_DIR/SeldonH.app/Contents/MacOS/SeldonH" || { echo "❌ L'exécutable n'est pas universel"; exit 1; }

echo "➡️  3b - Compilation de seldonh-mcp (serveur MCP, QtCore seul, universel) dans $MCP_BUILD_DIR"
mkdir -p "$MCP_BUILD_DIR"
rm -f "$MCP_BUILD_DIR/seldonh-mcp"
( cd "$MCP_BUILD_DIR" && "$QT_ROOT/bin/qmake" "$ROOT/mcp/seldonh-mcp.pro" "CONFIG+=release" "QMAKE_APPLE_DEVICE_ARCHS=$ARCHS" \
  && make -j"$NPROC" )
[ -x "$MCP_BUILD_DIR/seldonh-mcp" ] || { echo "❌ seldonh-mcp introuvable : $MCP_BUILD_DIR"; exit 1; }
is_universal "$MCP_BUILD_DIR/seldonh-mcp" || { echo "❌ seldonh-mcp n'est pas universel"; exit 1; }

echo "➡️  4 - Copie du bundle dans $STAGING"
rm -rf "$STAGING"
mkdir -p "$STAGING"
ditto "$BUILD_DIR/SeldonH.app" "$BUNDLE"      # ditto conserve liens symboliques et attributs étendus
cp "$MCP_BUILD_DIR/seldonh-mcp" "$MCP_EXE"

echo "➡️  5 - macdeployqt (frameworks et plugins Qt, dont tls pour le modèle distant ; seldonh-mcp aussi)"
# macdeployqt signe en "ad hoc" par défaut ; on re-signe tout à l'étape 7.
"$QT_ROOT/bin/macdeployqt" "$BUNDLE" "-executable=$MCP_EXE"
find "$BUNDLE" -name "*.cstemp" -delete

echo "➡️  6 - Info.plist (version)"
PLIST="$BUNDLE/Contents/Info.plist"
PB=/usr/libexec/PlistBuddy
plist_set() {   # $1 = clé, $2 = type, $3 = valeur
    "$PB" -c "Set :$1 $3" "$PLIST" 2>/dev/null || "$PB" -c "Add :$1 $2 $3" "$PLIST"
}
plist_set CFBundleVersion string "$VERSION"
plist_set CFBundleShortVersionString string "$VERSION"
plist_set CFBundleDisplayName string SeldonH
plist_set NSHumanReadableCopyright string "ChronoModel Team"

echo "➡️  7 - Signature (de l'intérieur vers l'extérieur, sans --deep)"
# Pas de --options runtime ni --timestamp (notarisation seulement) : avec une signature ad hoc, le
# "hardened runtime" ferait refuser le chargement des plugins Qt.
SIGN=(codesign --force --sign "$SIGN_ID")
find "$BUNDLE/Contents" -type f -name "*.dylib" -print0 | while IFS= read -r -d '' f; do
    "${SIGN[@]}" "$f"
done
if [ -d "$BUNDLE/Contents/Frameworks" ]; then
    for f in "$BUNDLE"/Contents/Frameworks/*.framework; do
        [ -d "$f" ] || continue
        "${SIGN[@]}" "$f"
    done
fi
"${SIGN[@]}" "$MCP_EXE"                         # exécutable secondaire : signé avant le bundle
"${SIGN[@]}" "$BUNDLE"

echo "➡️  8 - Vérifications"
codesign --verify --deep --strict --verbose=2 "$BUNDLE"
[ -d "$BUNDLE/Contents/Frameworks/QtCore.framework" ] || { echo "❌ QtCore.framework absent : macdeployqt a échoué"; exit 1; }
echo "   Dépendances restées en chemin absolu (doit être vide) :"
find "$BUNDLE/Contents" -type f \( -perm -u+x -o -name "*.dylib" \) -print0 \
    | xargs -0 otool -L 2>/dev/null \
    | grep -E "/usr/local/|/opt/homebrew/|/Users/" || echo "   (aucune)"
echo "   seldonh-mcp : $(lipo -archs "$MCP_EXE"), $("$MCP_EXE" --list | grep -c '"inputSchema"') outils"
"$MCP_EXE" --version >/dev/null || { echo "❌ seldonh-mcp ne démarre pas depuis le bundle"; exit 1; }
MACOS_MIN=$(otool -l "$EXE" 2>/dev/null | awk '/minos/{print $2; exit}')
if [ -n "${MACOS_MIN:-}" ]; then MACOS_TAG="macOS${MACOS_MIN%%.*}"; else MACOS_TAG="macOS"; fi
echo "   Cible macOS : $MACOS_TAG (minos $MACOS_MIN)"

echo "➡️  9 - Composant seldonh_app"
rm -rf "$DATA"
mkdir -p "$DATA"
ditto "$BUNDLE" "$DATA/SeldonH.app"
cp "$ROOT/LICENSE" "$ROOT/COPYING" "$DATA/"      # à côté du bundle (/Applications/SeldonH), hors signature
cp "$ROOT/mcp/LISEZMOI_MCP.txt" "$DATA/"         # chemin de seldonh-mcp écrit par installscript.qs
cp "$ROOT/third_party/llama.cpp/LICENSE" "$PKGS/seldonh_app/meta/llama.cpp-LICENSE.txt"
cp "$ROOT/third_party/qtkeychain/COPYING" "$PKGS/seldonh_app/meta/qtkeychain-LICENSE.txt"
cp "$ROOT/LICENSE" "$PKGS/seldonh_app/meta/SeldonH-LICENSE.txt"
cp "$ROOT/COPYING" "$PKGS/seldonh_app/meta/GPL-3.0.txt"

echo "➡️  10 - Composant seldonh_model"
EXCLUDE=()
SUFFIX=""
if [ -z "$MODEL" ]; then
    echo "   pas de modèle embarqué"
    EXCLUDE=(--exclude seldonh_model)
    SUFFIX="_noModel"
else
    mkdir -p "$MODEL_DIR"
    find "$MODEL_DIR" -name '*.gguf' ! -name "$MODEL" -delete   # un seul .gguf (SeldonH charge le premier)
    if [ "$(stat -f %z "$ROOT/models/$MODEL")" != "$(stat -f %z "$MODEL_DIR/$MODEL" 2>/dev/null || echo -1)" ]; then
        echo "   copie de $MODEL …"
        cp "$ROOT/models/$MODEL" "$MODEL_DIR/"
    else
        echo "   $MODEL déjà en place"
    fi
fi

echo "➡️  11 - Version $VERSION et date $DATE_ISO dans config.xml / package.xml"
sed -i '' -E "s|<Version>[^<]*</Version>|<Version>$VERSION</Version>|; s|<ReleaseDate>[^<]*</ReleaseDate>|<ReleaseDate>$DATE_ISO</ReleaseDate>|" \
    "$INST/installer-config/config.xml" "$PKGS/seldonh_app/meta/package.xml" "$PKGS/seldonh_model/meta/package.xml"

INSTALLER="SeldonH_v${VERSION}_${QT_VER_STR}_${MACOS_TAG}_${DATE_FILE}_Installer${SUFFIX}"
echo "➡️  12 - binarycreator : $INSTALLER.app"
AC=()
"$IFW/binarycreator" --help 2>&1 | grep -q -- '--ac' && AC=(--ac "$COMPRESSION")
cd "$INST"
rm -rf "$INSTALLER.app"
# ${X[@]+"${X[@]}"} : tableaux éventuellement vides avec set -u sous le bash 3.2 de macOS
"$IFW/binarycreator" --offline-only ${AC[@]+"${AC[@]}"} ${EXCLUDE[@]+"${EXCLUDE[@]}"} \
    -c installer-config/config.xml -p installer-packages-macOS "$INSTALLER"
# Sans signature, l'installeur ne se lance pas sur Apple Silicon.
codesign --force --deep --sign "$SIGN_ID" "$INSTALLER.app"
codesign --verify --deep --strict --verbose=2 "$INSTALLER.app"

if [ "$MAKE_DMG" = 1 ]; then
    echo "➡️  13 - Création du .dmg"
    DMG="$INST/$INSTALLER.dmg"
    DMG_DIR="$INST/dmg_content"
    rm -rf "$DMG_DIR" "$DMG"
    mkdir -p "$DMG_DIR"
    ditto "$INSTALLER.app" "$DMG_DIR/$INSTALLER.app"
    cat > "$DMG_DIR/LISEZMOI.txt" <<LISEZMOI
SeldonH - installation sur macOS

Cette application n'est pas signée avec un certificat Apple payant : macOS affiche donc
un avertissement à la première ouverture (« Apple ne peut pas vérifier... »). Ce n'est pas
le signe d'un problème, il suffit d'autoriser l'installeur une seule fois :

1. Copiez l'installeur du disque vers votre Bureau ou vos Documents.
2. Double-cliquez dessus : macOS refuse de l'ouvrir. Cliquez sur « Terminé » (ou « OK »).
3. Ouvrez Réglages Système > Confidentialité et sécurité, descendez jusqu'à la section
   « Sécurité » et cliquez sur « Ouvrir quand même » à côté du nom de l'installeur.
   Confirmez avec votre mot de passe, puis cliquez sur « Ouvrir ».

Alternative dans le Terminal (adapter le chemin) :
   xattr -dr com.apple.quarantine "/chemin/vers/$INSTALLER.app"

Sur macOS 14 et antérieur, clic droit sur l'installeur > Ouvrir suffit généralement.

SeldonH est installé dans /Applications/SeldonH (avec le modèle de langage dans le
sous-dossier models). Si macOS refuse aussi d'ouvrir SeldonH, utilisez la même procédure.

Serveur MCP (calculs de SeldonH dans Claude Desktop, Jan, Gemini CLI…) :
/Applications/SeldonH/SeldonH.app/Contents/MacOS/seldonh-mcp ; voir
/Applications/SeldonH/LISEZMOI_MCP.txt après l'installation.

Clé API d'un modèle distant : SeldonH la range dans le trousseau de macOS. À la première
utilisation, et après chaque mise à jour de SeldonH, macOS peut demander si SeldonH peut
utiliser votre trousseau : saisissez votre mot de passe de session et cliquez sur
« Toujours autoriser ».
LISEZMOI
    hdiutil create -volname "$INSTALLER" -srcfolder "$DMG_DIR" -ov -format UDZO "$DMG"
    rm -rf "$DMG_DIR"
    hdiutil verify "$DMG"
    echo "✅ 14 - Terminé : $DMG"
else
    echo "✅ 13 - Terminé : $INST/$INSTALLER.app"
fi
