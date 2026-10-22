@echo off
rem ---------------------------------------------------------------------------
rem  SeldonH - compile llama.cpp (Windows, MinGW du kit Qt) et l'installe dans
rem  third_party\llama-install (lu par llama.pri) :
rem     bin\  libllama.dll, ggml.dll, ggml-base.dll + backends charges au demarrage
rem     lib\  bibliotheques d'importation (*.dll.a) pour l'edition de liens
rem
rem  Backends dynamiques (GGML_BACKEND_DL), comme les versions officielles de llama.cpp :
rem    - CPU : une DLL par jeu d'instructions (ggml-cpu-x64, -sse42, -haswell, -skylakex, -zen4...),
rem            la meilleure est choisie au lancement : fonctionne sur tous les PC 64 bits ;
rem    - GPU : ggml-vulkan.dll (NVIDIA, AMD, Intel), ignoree si le PC n'a pas de pilote Vulkan.
rem  => un seul installeur, rien a compiler pour l'utilisateur.
rem
rem  Usage :  scripts\build_llama_win.bat
rem           set VULKAN=OFF & scripts\build_llama_win.bat     (sans GPU)
rem
rem  Vulkan : installer le Vulkan SDK sur la machine de compilation
rem           (https://vulkan.lunarg.com/sdk/home#windows) ; il definit VULKAN_SDK.
rem  OpenMP desactive : ggml utilise son propre pool de threads, pas de libgomp-1.dll a livrer.
rem ---------------------------------------------------------------------------
setlocal

rem ---------------------------- A VERIFIER ----------------------------------
set QT_TOOLS=C:\Qt\Tools
set MINGW=%QT_TOOLS%\mingw1310_64
set CMAKE_BIN=%QT_TOOLS%\CMake_64\bin
if "%LLAMA_COMMIT%"=="" set LLAMA_COMMIT=f849095
if "%VULKAN%"=="" set VULKAN=ON
rem --------------------------------------------------------------------------

for %%I in ("%~dp0..") do set ROOT=%%~fI
set SRC=%ROOT%\third_party\llama.cpp
set BUILD=%ROOT%\third_party\llama-build-win
set PREFIX=%ROOT%\third_party\llama-install
set PATH=%MINGW%\bin;%CMAKE_BIN%;%PATH%

if not exist "%MINGW%\bin\g++.exe" ( echo ERREUR : MinGW introuvable dans %MINGW% & exit /b 1 )
where cmake >nul 2>nul || ( echo ERREUR : cmake introuvable ^(%CMAKE_BIN%^) & exit /b 1 )
where git   >nul 2>nul || ( echo ERREUR : git introuvable dans le PATH & exit /b 1 )
if /i "%VULKAN%"=="ON" if not defined VULKAN_SDK (
    echo ERREUR : VULKAN_SDK non defini - installer le Vulkan SDK, ou "set VULKAN=OFF"
    exit /b 1
)

echo  1 - Sources llama.cpp (commit %LLAMA_COMMIT%)
if not exist "%SRC%\.git" ( git clone https://github.com/ggml-org/llama.cpp "%SRC%" || exit /b 1 )
git -C "%SRC%" fetch --quiet origin
rem annule une eventuelle compatibilite MinGW appliquee lors d'une compilation precedente
git -C "%SRC%" checkout --quiet -- ggml/src/ggml-cpu/ggml-cpu.c
git -C "%SRC%" checkout --quiet %LLAMA_COMMIT% || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0llama_mingw_compat.ps1" "%SRC%\ggml\src\ggml-cpu\ggml-cpu.c"
if errorlevel 1 exit /b 1

echo  2 - Configuration CMake (backends dynamiques, Vulkan %VULKAN%)
cmake -S "%SRC%" -B "%BUILD%" -G "MinGW Makefiles" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON ^
  -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_CUDA=OFF -DGGML_VULKAN=%VULKAN% ^
  -DLLAMA_CURL=OFF -DLLAMA_BUILD_COMMON=OFF ^
  -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF
if errorlevel 1 exit /b 1

echo  3 - Compilation (les shaders Vulkan prennent plusieurs minutes)
cmake --build "%BUILD%" --target llama --config Release -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 exit /b 1

echo  4 - Installation dans %PREFIX%
if exist "%PREFIX%" rmdir /s /q "%PREFIX%"
mkdir "%PREFIX%\include"
mkdir "%PREFIX%\lib"
mkdir "%PREFIX%\bin"
copy /y "%SRC%\include\llama.h"     "%PREFIX%\include\" >nul
copy /y "%SRC%\include\llama-cpp.h" "%PREFIX%\include\" >nul
copy /y "%SRC%\ggml\include\*.h"    "%PREFIX%\include\" >nul
copy /y "%BUILD%\bin\*.dll" "%PREFIX%\bin\" >nul
for /r "%BUILD%" %%F in (*.dll.a) do copy /y "%%F" "%PREFIX%\lib\" >nul
> "%PREFIX%\BUILD_INFO.txt" (
    echo os=Windows
    echo variant=dl-vulkan-%VULKAN%
    echo commit=%LLAMA_COMMIT%
)

echo OK : llama.cpp installe dans %PREFIX%
dir /b "%PREFIX%\bin" "%PREFIX%\lib"
endlocal
exit /b 0
