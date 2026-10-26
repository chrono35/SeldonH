# Liaison avec QtKeychain (clé API rangée dans le trousseau du système), compilé par :
#   macOS   : scripts/build_qtkeychain.sh        -> bibliothèque statique, universelle (Trousseau macOS)
#   Linux   : scripts/build_qtkeychain.sh        -> libqt6keychain.so (Secret Service : GNOME Keyring, KWallet…)
#   Windows : scripts\build_qtkeychain_win.bat   -> libqt6keychain.dll (Gestionnaire d'identifiants), MinGW du kit Qt
# Résultat dans third_party/qtkeychain-install/{include,lib[,bin]}.
# Sans cette étape SeldonH se compile quand même, mais la clé API reste en clair dans les réglages
# (SecretStore le signale dans les Préférences).

QTKEYCHAIN_PREFIX = $$PWD/third_party/qtkeychain-install

exists($$QTKEYCHAIN_PREFIX/include/qt6keychain/keychain.h) {
    DEFINES += SELDON_HAS_QTKEYCHAIN
    INCLUDEPATH += $$QTKEYCHAIN_PREFIX/include
    LIBS += -L$$QTKEYCHAIN_PREFIX/lib -lqt6keychain
    macx {
        # Bibliothèque statique : frameworks système qu'elle utilise.
        LIBS += -framework Foundation -framework Security
    } else: win32 {
        # Développement : libqt6keychain.dll doit être à côté de l'exécutable pour qu'il démarre.
        QTKEYCHAIN_DLL_DEST = $$OUT_PWD
        debug_and_release {
            CONFIG(debug, debug|release): QTKEYCHAIN_DLL_DEST = $$OUT_PWD/debug
            else: QTKEYCHAIN_DLL_DEST = $$OUT_PWD/release
        }
        QMAKE_POST_LINK += $$QMAKE_COPY $$shell_quote($$shell_path($$QTKEYCHAIN_PREFIX/bin/*.dll)) $$shell_quote($$shell_path($$QTKEYCHAIN_DLL_DEST)) $$escape_expand(\\n\\t)
    } else {
        QMAKE_RPATHDIR += $$QTKEYCHAIN_PREFIX/lib     # développement ; remplacé par $ORIGIN/lib au déploiement
    }
} else {
    warning("QtKeychain introuvable dans $$QTKEYCHAIN_PREFIX : la clé API sera enregistrée en clair. Exécutez scripts/build_qtkeychain.sh (ou scripts\\build_qtkeychain_win.bat)")
}
