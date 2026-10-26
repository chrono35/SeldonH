#!/bin/bash
# ===========================================================================
#  SeldonH - QtIFW_script_deb_noModel.sh   (Linux Debian / Ubuntu, x86_64)
#  Installeur SANS modèle de langage (~100 Mo) :
#     SeldonH_v<version>_Qt6_x_x_linux64_<AAAAMMJJ>_Installer_noModel.run
#  L'utilisateur choisit ensuite un modèle distant (API) ou son propre .gguf.
#  Appelle QtIFW_script_deb.sh avec l'argument noModel ; les autres arguments (rebuild) sont transmis.
#
#  Usage :  bash QtInstaller_SeldonH/QtIFW_script_deb_noModel.sh [rebuild]
# ===========================================================================
exec bash "$(dirname "$0")/QtIFW_script_deb.sh" noModel "$@"
