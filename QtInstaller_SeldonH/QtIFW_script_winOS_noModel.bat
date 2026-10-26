@echo off
rem ===========================================================================
rem  SeldonH - QtIFW_script_winOS_noModel.bat
rem  Installeur Windows SANS modele de langage (~80 Mo) :
rem     SeldonH_v<version>_Qt6_x_x_win64_<AAAAMMJJ>_Installer_noModel.exe
rem  L'utilisateur choisit ensuite un modele distant (API) ou son propre .gguf.
rem  Appelle QtIFW_script_winOS.bat avec l'argument noModel (meme compilation,
rem  memes bibliotheques) ; les autres arguments (rebuild) sont transmis.
rem
rem  Usage :  QtInstaller_SeldonH\QtIFW_script_winOS_noModel.bat [rebuild]
rem ===========================================================================
call "%~dp0QtIFW_script_winOS.bat" noModel %*
exit /b %errorlevel%
