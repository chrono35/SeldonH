@echo off
rem ===========================================================================
rem  SeldonH - QtIFW_script_winOS.bat
rem  Compile SeldonH (Release, MinGW), rassemble les DLL Qt (windeployqt),
rem  embarque le modele .gguf et construit l'installeur Qt Installer Framework.
rem
rem  Usage (depuis n'importe quel dossier) :
rem     QtInstaller_SeldonH\QtIFW_script_winOS.bat               installeur complet
rem     QtInstaller_SeldonH\QtIFW_script_winOS.bat rebuild       force la recompilation de llama.cpp et de QtKeychain
rem     QtInstaller_SeldonH\QtIFW_script_winOS.bat nomodel       installeur sans modele (~80 Mo)
rem     set MODEL=Qwen3.5-4B-Q3_K_M.gguf  (avant l'appel) pour embarquer un autre modele
rem
rem  Un seul installeur pour tous les PC : llama.cpp est livre avec ses backends
rem  charges au demarrage (CPU : meilleure variante du processeur ; GPU : Vulkan si
rem  le pilote graphique le permet). Rien a compiler pour l'utilisateur.
rem  llama.cpp est recompile automatiquement (scripts\build_llama_win.bat) s'il est absent.
rem  Cle API : QtKeychain (libqt6keychain.dll, scripts\build_qtkeychain_win.bat) la range dans le
rem  Gestionnaire d'identifiants de Windows.
rem
rem  Project ChronoModel Team - https://chronomodel.com/
rem  author email: philippe.dufresne@univ-rennes.fr
rem  date 2026-10-22
rem ===========================================================================
setlocal
cls

rem ---------------------------- A VERIFIER ----------------------------------
set VERSION=1.0.0
set QTDIR=C:\Qt\6.11.2\mingw_64
set MINGW=C:\Qt\Tools\mingw1310_64
set IFW=C:\Qt\Tools\QtInstallerFramework\4.10\bin
rem Modele embarque (dans le dossier models du projet). Vide = installeur sans modele.
if not defined MODEL set MODEL=Qwen3.5-4B-Q4_K_M.gguf
rem Compression 7z des archives (0 = aucune : un .gguf ne se compresse pas, et 2,7 Go
rem en LZMA prennent tres longtemps pour quelques % de gain).
if not defined COMPRESSION set COMPRESSION=0
rem --------------------------------------------------------------------------

set REBUILD=0
for %%a in (%*) do (
    if /i "%%a"=="rebuild" set REBUILD=1
    if /i "%%a"=="nomodel" set MODEL=
)

for %%I in ("%~dp0..") do set ROOT=%%~fI
set INST=%ROOT%\QtInstaller_SeldonH
set PKGS=%INST%\installer-packages-winOS
set APPDATA_DIR=%PKGS%\seldonh.app\data
set MODEL_DIR=%PKGS%\seldonh.model\data\models
set BUILD_DIR=%ROOT%\build\win64-Release
set LLAMA=%ROOT%\third_party\llama-install
set KEYCHAIN=%ROOT%\third_party\qtkeychain-install
set PATH=%QTDIR%\bin;%MINGW%\bin;%PATH%

for %%A in ("%QTDIR%\..") do set QT_NUM=%%~nxA
for /f "tokens=1-3 delims=." %%a in ("%QT_NUM%") do set QT_VER_STR=Qt%%a_%%b_%%c
for /f %%d in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd"') do set DATE_STR=%%d
for /f %%d in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"') do set DATE_ISO=%%d

echo  1 - Verification des chemins
if not exist "%QTDIR%\bin\qmake.exe"        ( echo ERREUR : Qt introuvable dans %QTDIR% & goto :fail )
if not exist "%QTDIR%\bin\windeployqt.exe"  ( echo ERREUR : windeployqt introuvable dans %QTDIR%\bin & goto :fail )
if not exist "%MINGW%\bin\mingw32-make.exe" ( echo ERREUR : MinGW introuvable dans %MINGW% & goto :fail )
if not exist "%IFW%\binarycreator.exe"      ( echo ERREUR : QtIFW introuvable dans %IFW% & goto :fail )
if defined MODEL if not exist "%ROOT%\models\%MODEL%" (
    echo ERREUR : modele introuvable : %ROOT%\models\%MODEL%
    echo         ^(le telecharger, ou "set MODEL=" pour un installeur sans modele^)
    goto :fail
)

echo  2 - llama.cpp (backends dynamiques CPU + Vulkan)
set NEED_LLAMA=0
if "%REBUILD%"=="1" set NEED_LLAMA=1
if not exist "%LLAMA%\bin\libllama.dll" set NEED_LLAMA=1
if not exist "%LLAMA%\lib\libllama.dll.a" set NEED_LLAMA=1
if "%NEED_LLAMA%"=="1" (
    echo     compilation de llama.cpp
    call "%ROOT%\scripts\build_llama_win.bat"
    if errorlevel 1 ( echo ERREUR : compilation de llama.cpp & goto :fail )
) else (
    type "%LLAMA%\BUILD_INFO.txt"
)

echo  2b - QtKeychain (cle API dans le Gestionnaire d'identifiants)
set NEED_KEYCHAIN=0
if "%REBUILD%"=="1" set NEED_KEYCHAIN=1
if not exist "%KEYCHAIN%\bin\libqt6keychain.dll" set NEED_KEYCHAIN=1
if not exist "%KEYCHAIN%\lib\libqt6keychain.dll.a" set NEED_KEYCHAIN=1
if "%NEED_KEYCHAIN%"=="1" (
    echo     compilation de QtKeychain
    call "%ROOT%\scripts\build_qtkeychain_win.bat"
    if errorlevel 1 ( echo ERREUR : compilation de QtKeychain & goto :fail )
) else (
    type "%KEYCHAIN%\BUILD_INFO.txt"
)

echo  3 - Compilation de SeldonH (Release) dans %BUILD_DIR%
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
pushd "%BUILD_DIR%"
rem qmake relance a chaque fois : llama.pri choisit les bibliotheques (Vulkan ou non) a cette etape.
rem L'executable est supprime pour forcer l'edition de liens avec les bibliotheques llama.cpp actuelles.
if exist SeldonH.exe del /q SeldonH.exe
if exist release\SeldonH.exe del /q release\SeldonH.exe
"%QTDIR%\bin\qmake.exe" "%ROOT%\SeldonH.pro" -spec win32-g++ "CONFIG+=release" "CONFIG-=debug_and_release"
if errorlevel 1 ( popd & echo ERREUR : qmake & goto :fail )
mingw32-make -j%NUMBER_OF_PROCESSORS%
if errorlevel 1 ( popd & echo ERREUR : compilation & goto :fail )
popd
set EXE=%BUILD_DIR%\SeldonH.exe
if not exist "%EXE%" set EXE=%BUILD_DIR%\release\SeldonH.exe
if not exist "%EXE%" ( echo ERREUR : SeldonH.exe introuvable dans %BUILD_DIR% & goto :fail )

echo  4 - Preparation du composant seldonh.app
if exist "%APPDATA_DIR%" rmdir /s /q "%APPDATA_DIR%"
mkdir "%APPDATA_DIR%"
copy /y "%EXE%" "%APPDATA_DIR%\" >nul
copy /y "%ROOT%\resources\icons\SeldonH.ico" "%APPDATA_DIR%\" >nul
copy /y "%ROOT%\third_party\llama.cpp\LICENSE" "%PKGS%\seldonh.app\meta\llama.cpp-LICENSE.txt" >nul
copy /y "%ROOT%\third_party\qtkeychain\COPYING" "%PKGS%\seldonh.app\meta\qtkeychain-LICENSE.txt" >nul
rem llama.cpp : libllama.dll, ggml.dll, ggml-base.dll et backends ggml-cpu-*.dll, ggml-vulkan.dll
rem (vulkan-1.dll n'est PAS livree : elle vient du pilote graphique ; sans elle, ggml-vulkan.dll est ignoree)
copy /y "%LLAMA%\bin\*.dll" "%APPDATA_DIR%\" >nul
if errorlevel 1 ( echo ERREUR : DLL llama.cpp introuvables dans %LLAMA%\bin & goto :fail )
dir /b "%APPDATA_DIR%\ggml*.dll"
rem QtKeychain : libqt6keychain.dll (ses DLL Qt sont ajoutees par windeployqt, crypt32 fait partie de Windows)
copy /y "%KEYCHAIN%\bin\*.dll" "%APPDATA_DIR%\" >nul
if errorlevel 1 ( echo ERREUR : DLL QtKeychain introuvable dans %KEYCHAIN%\bin & goto :fail )

echo  5 - windeployqt (DLL Qt, plugins platforms / styles / imageformats / tls ...)
"%QTDIR%\bin\windeployqt.exe" --release --compiler-runtime --no-translations --no-system-d3d-compiler --no-opengl-sw "%APPDATA_DIR%\SeldonH.exe"
if errorlevel 1 ( echo ERREUR : windeployqt & goto :fail )
rem Runtime GCC : copie explicite, au cas ou windeployqt ne trouverait pas la toolchain
for %%L in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
    if not exist "%APPDATA_DIR%\%%L" copy /y "%MINGW%\bin\%%L" "%APPDATA_DIR%\" >nul
)

echo  6 - Composant seldonh.model
set EXCLUDE=
if not defined MODEL (
    echo     pas de modele embarque
    set EXCLUDE=--exclude seldonh.model
    set SUFFIX=_sansModele
    goto :model_done
)
set SUFFIX=
if not exist "%MODEL_DIR%" mkdir "%MODEL_DIR%"
rem Un seul .gguf dans le composant (SeldonH charge le premier par ordre alphabetique)
for %%G in ("%MODEL_DIR%\*.gguf") do if /i not "%%~nxG"=="%MODEL%" del /q "%%G"
rem 2,7 Go : recopie seulement si absent ou de taille differente
set SRC_SIZE=0
set DST_SIZE=-1
for %%G in ("%ROOT%\models\%MODEL%") do set SRC_SIZE=%%~zG
if exist "%MODEL_DIR%\%MODEL%" for %%G in ("%MODEL_DIR%\%MODEL%") do set DST_SIZE=%%~zG
if not "%SRC_SIZE%"=="%DST_SIZE%" (
    echo     copie de %MODEL% ...
    copy /y "%ROOT%\models\%MODEL%" "%MODEL_DIR%\" >nul
) else (
    echo     %MODEL% deja en place
)
:model_done

echo  7 - Version %VERSION% et date %DATE_ISO% dans config.xml / package.xml
powershell -NoProfile -Command ^
  "$files = @('%INST%\installer-config\config.xml','%PKGS%\seldonh.app\meta\package.xml','%PKGS%\seldonh.model\meta\package.xml');" ^
  "foreach ($f in $files) { $t = [IO.File]::ReadAllText($f);" ^
  "  $t = $t -replace '<Version>[^<]*</Version>', '<Version>%VERSION%</Version>' -replace '<ReleaseDate>[^<]*</ReleaseDate>', '<ReleaseDate>%DATE_ISO%</ReleaseDate>';" ^
  "  [IO.File]::WriteAllText($f, $t, (New-Object Text.UTF8Encoding $false)) }"

set INSTALLER=SeldonH_v%VERSION%_%QT_VER_STR%_win64_%DATE_STR%_Installer%SUFFIX%
echo  8 - binarycreator : %INSTALLER%.exe
set AC=
"%IFW%\binarycreator.exe" --help 2>&1 | findstr /c:"--ac" >nul && set AC=--ac %COMPRESSION%
pushd "%INST%"
"%IFW%\binarycreator.exe" --offline-only %AC% %EXCLUDE% -c installer-config\config.xml -p installer-packages-winOS "%INSTALLER%"
if errorlevel 1 ( popd & echo ERREUR : binarycreator & goto :fail )
popd

echo  9 - Termine : %INST%\%INSTALLER%.exe
pause
endlocal
exit /b 0

:fail
echo.
echo *** Echec du deploiement ***
pause
endlocal
exit /b 1
