#!/bin/bash
# ===========================================================================
#  SeldonH - QtIFW_script_macOS_noModel.sh   (macOS universel x86_64 + arm64)
#  Installeur SANS modèle de langage :
#     SeldonH_v<version>_Qt6_x_x_macOS<minos>_<AAAAMMJJ>_Installer_noModel.app (+ .dmg)
#  L'utilisateur choisit ensuite un modèle distant (API) ou son propre .gguf.
#  Appelle QtIFW_script_macOS.sh avec l'argument noModel ; les autres arguments (rebuild, nodmg) sont transmis.
#
#  Usage :  bash QtInstaller_SeldonH/QtIFW_script_macOS_noModel.sh [rebuild] [nodmg]
# ===========================================================================
exec bash "$(dirname "$0")/QtIFW_script_macOS.sh" noModel "$@"
