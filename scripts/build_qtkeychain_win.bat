@echo off
rem ---------------------------------------------------------------------------
rem  SeldonH - compile QtKeychain (Windows, MinGW du kit Qt) et l'installe dans
rem  third_party\qtkeychain-install (lu par qtkeychain.pri) :
rem     bin\      libqt6keychain.dll  (livree a cote de SeldonH.exe)
rem     lib\      libqt6keychain.dll.a (bibliotheque d'importation)
rem     include\  qt6keychain\keychain.h
rem
rem  La cle API est rangee dans le Gestionnaire d'identifiants de Windows
rem  (Credential Store) : aucune autre DLL a livrer (crypt32 fait partie de Windows).
rem
rem  Usage :  scripts\build_qtkeychain_win.bat
rem           set QTDIR=C:\Qt\6.11.2\mingw_64 & scripts\build_qtkeychain_win.bat
rem ---------------------------------------------------------------------------
setlocal

rem ---------------------------- A VERIFIER ----------------------------------
set QT_TOOLS=C:\Qt\Tools
set MINGW=%QT_TOOLS%\mingw1310_64
set CMAKE_BIN=%QT_TOOLS%\CMake_64\bin
if "%QTDIR%"=="" set QTDIR=C:\Qt\6.11.2\mingw_64
if "%QTKEYCHAIN_TAG%"=="" set QTKEYCHAIN_TAG=0.17.0
rem --------------------------------------------------------------------------

for %%I in ("%~dp0..") do set ROOT=%%~fI
set SRC=%ROOT%\third_party\qtkeychain
set BUILD=%ROOT%\third_party\qtkeychain-build-win
set PREFIX=%ROOT%\third_party\qtkeychain-install
set PATH=%MINGW%\bin;%CMAKE_BIN%;%QTDIR%\bin;%PATH%

if not exist "%MINGW%\bin\g++.exe" ( echo ERREUR : MinGW introuvable dans %MINGW% & exit /b 1 )
if not exist "%QTDIR%\lib\cmake\Qt6\Qt6Config.cmake" ( echo ERREUR : Qt 6 introuvable dans %QTDIR% ^(variable QTDIR^) & exit /b 1 )
where cmake >nul 2>nul || ( echo ERREUR : cmake introuvable ^(%CMAKE_BIN%^) & exit /b 1 )
where git   >nul 2>nul || ( echo ERREUR : git introuvable dans le PATH & exit /b 1 )

echo  1 - Sources QtKeychain (%QTKEYCHAIN_TAG%)
if not exist "%SRC%\.git" ( git clone https://github.com/frankosterfeld/qtkeychain "%SRC%" || exit /b 1 )
git -C "%SRC%" fetch --quiet --tags origin
git -C "%SRC%" checkout --quiet %QTKEYCHAIN_TAG% || exit /b 1

echo  2 - Configuration CMake (bibliotheque partagee, Credential Store)
cmake -S "%SRC%" -B "%BUILD%" -G "MinGW Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="%QTDIR%" ^
  -DCMAKE_INSTALL_PREFIX="%PREFIX%" ^
  -DCMAKE_INSTALL_LIBDIR=lib ^
  -DBUILD_WITH_QT5=OFF -DBUILD_SHARED_LIBS=ON ^
  -DBUILD_TRANSLATIONS=OFF -DBUILD_TEST_APPLICATION=OFF -DBUILD_TESTING=OFF ^
  -DUSE_CREDENTIAL_STORE=ON
if errorlevel 1 exit /b 1

echo  3 - Compilation
cmake --build "%BUILD%" --config Release -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b 1

echo  4 - Installation dans %PREFIX%
if exist "%PREFIX%" rmdir /s /q "%PREFIX%"
cmake --install "%BUILD%" --config Release
if errorlevel 1 exit /b 1
> "%PREFIX%\BUILD_INFO.txt" (
    echo os=Windows
    echo variant=shared-credentialstore
    echo tag=%QTKEYCHAIN_TAG%
    echo qt=%QTDIR%
)

echo OK : QtKeychain installe dans %PREFIX%
dir /b "%PREFIX%\bin" "%PREFIX%\lib"
endlocal
exit /b 0
