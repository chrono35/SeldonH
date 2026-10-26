# Déploiement de SeldonH (Qt Installer Framework)

**Un installeur par système**, valable pour toutes les machines (avec ou sans GPU) : l'utilisateur n'a rien à compiler.
Même organisation que `ChronoModel/QtInstaller_ChronoModel`, mais chaque script fait tout :
llama.cpp et QtKeychain (si besoin) → compilation Release de SeldonH → bibliothèques Qt → modèle `.gguf` → `binarycreator`.

| Système | Script | Résultat |
|---|---|---|
| Windows 64 bits (MinGW) | `QtInstaller_SeldonH\QtIFW_script_winOS.bat [rebuild] [noModel]` | `SeldonH_v1.0.0_Qt6_11_2_win64_AAAAMMJJ_Installer.exe` |
| Debian / Ubuntu x86_64 | `bash QtInstaller_SeldonH/QtIFW_script_deb.sh [rebuild] [noModel]` | `SeldonH_v1.0.0_Qt6_11_2_linux64_AAAAMMJJ_Installer.run` |
| macOS universel (x86_64 + arm64) | `bash QtInstaller_SeldonH/QtIFW_script_macOS.sh [rebuild] [noModel] [nodmg]` | `SeldonH_v1.0.0_Qt6_11_1_macOS13_AAAAMMJJ_Installer.app` + `.dmg` |

**Installeurs sans modèle** (`_noModel`, ~80-100 Mo : l'utilisateur choisit un modèle distant ou son propre `.gguf`) :
`QtIFW_script_winOS_noModel.bat`, `QtIFW_script_deb_noModel.sh`, `QtIFW_script_macOS_noModel.sh`.
Ils appellent le script principal avec l'argument `noModel` (les autres arguments sont transmis).

Les chemins (Qt, MinGW, QtIFW), la version et le modèle sont en tête de chaque script, bloc **A VERIFIER**.

## CPU et GPU dans le même installeur

* **Windows et Linux** : llama.cpp est compilé avec ses *backends chargés au démarrage* (`GGML_BACKEND_DL`),
  comme les versions officielles de llama.cpp. L'installeur contient :
  * `ggml-cpu-x64`, `-sse42`, `-sandybridge`, `-haswell`, `-skylakex`, `-icelake`, `-alderlake`, `-zen4`… :
    SeldonH charge la meilleure variante pour le processeur (fonctionne aussi sur les vieux PC) ;
  * `ggml-vulkan` : GPU NVIDIA, AMD ou Intel. Il n'est chargé que si le pilote graphique fournit
    `vulkan-1.dll` / `libvulkan.so.1` ; sinon il est ignoré et SeldonH tourne sur le processeur.
  Le chargement est fait par `LlmCore` (dossier de l'exécutable sous Windows, `lib/` sous Linux).
* **macOS** : binaire universel avec Metal ; GPU sur Apple Silicon, processeur sur Mac Intel (réglage par défaut).
* Dans SeldonH : Préférences > « Utiliser le GPU » (activé par défaut, sauf Mac Intel).

## Clé API : trousseau du système (QtKeychain)

La clé API d'un modèle distant n'est pas enregistrée dans les réglages : `SecretStore` la range dans le trousseau
du système grâce à [QtKeychain](https://github.com/frankosterfeld/qtkeychain) (licence BSD 2 clauses, version 0.17.0),
compilé par `scripts/build_qtkeychain.sh` (macOS, Linux) ou `scripts\build_qtkeychain_win.bat` (Windows) dans
`third_party/qtkeychain-install`, puis lié par `qtkeychain.pri`. Les scripts QtIFW le compilent s'il est absent
(argument `rebuild` : recompilé avec llama.cpp). Compilation vérifiée sous macOS et Windows ; reste à faire sous Linux.

| Système | Trousseau | Livré dans l'installeur |
|---|---|---|
| macOS | Trousseau macOS | rien (bibliothèque statique, universelle) |
| Windows | Gestionnaire d'identifiants | `libqt6keychain.dll` à côté de `SeldonH.exe` |
| Linux | Secret Service (GNOME Keyring, KWallet, KeePassXC…) | `lib/libqt6keychain.so*` (+ `libQt6DBus`) ; libsecret reste celle du système |

* Sans trousseau joignable (Linux en session minimale ou par SSH), la clé est enregistrée en clair dans les réglages
  et les Préférences l'indiquent sous le champ « Clé API ».
* Si `third_party/qtkeychain-install` manque à la compilation, `qmake` affiche un avertissement et SeldonH est
  construit sans trousseau (clé en clair, signalée de la même façon).
* macOS : la signature étant ad hoc, macOS redemande l'accès au trousseau après chaque mise à jour de SeldonH
  (« Toujours autoriser ») ; le LISEZMOI du `.dmg` le mentionne. Une signature Developer ID supprimerait cette demande.
* Préférences : une Base URL en `http://` vers une autre machine que celle-ci, avec une clé, affiche un avertissement
  et demande confirmation à l'enregistrement (la clé circulerait en clair).

## Licences affichées par l'installeur

SeldonH est sous **GPL-3.0-or-later** (comme ChronoModel). Les scripts copient dans `seldonh_app/meta/` :
`SeldonH-LICENSE.txt` (= `LICENSE`, notice et composants tiers), `GPL-3.0.txt` (= `COPYING`),
`llama.cpp-LICENSE.txt` (MIT) et `qtkeychain-LICENSE.txt` (BSD 2 clauses), déclarés dans `package.xml` (`<Licenses>`).
`LICENSE` et `COPYING` sont aussi installés à côté de l'application.

## Contenu de l'installeur

* `seldonh_app` (obligatoire) : exécutable, Qt, llama.cpp, QtKeychain, `LICENSE` + `COPYING`, raccourcis (menu Démarrer + Bureau sous Windows,
  entrée de menu `~/.local/share/applications` sous Linux).
* `seldonh_model` (coché par défaut, décochable) : `Qwen3.5-4B-Q4_K_M.gguf` dans `models/`.
  Autre modèle : `MODEL=Qwen3.5-4B-Q3_K_M.gguf`.
* Dossier d'installation : `C:\Program Files\SeldonH` ; Linux `~/SeldonH` (pas de root, lanceur `SeldonH.sh`) ;
  macOS `/Applications/SeldonH/SeldonH.app`, modèle dans `/Applications/SeldonH/models`
  (hors du bundle pour ne pas invalider la signature).
* Modèle distant par défaut (Préférences, API compatible OpenAI, sans clé) :
  `https://oai.endpoints.kepler.ai.cloud.ovh.net/v1`, modèle `Mistral-Small-3.2-24B-Instruct-2506`.

## macOS : copie des bibliothèques et signature

Comme `Ch_copy_library_and_signature.sh` + `QtIFW_script_macOS.sh` + `Ch_make_dmg_macOS.sh` de ChronoModel :
compilation universelle (`QMAKE_APPLE_DEVICE_ARCHS="x86_64 arm64"`, llama.cpp universel) → copie du bundle dans
`staging/` → `macdeployqt` → Info.plist → signature ad hoc de l'intérieur vers l'extérieur → vérifications
(signature, chemins absolus, version macOS minimale) → `binarycreator` → signature de l'installeur → `.dmg` + LISEZMOI.

## Pré-requis (machine de compilation seulement)

* Windows : Qt 6.11.x MinGW 64, `C:\Qt\Tools\mingw1310_64`, CMake du kit Qt, git, QtIFW 4.10+,
  **Vulkan SDK** (https://vulkan.lunarg.com, définit `VULKAN_SDK`).
* Debian : Qt 6.11.x `gcc_64`, QtIFW,
  `sudo apt install build-essential cmake git patchelf libgl1-mesa-dev libxkbcommon-dev libvulkan-dev glslc spirv-headers pkg-config libsecret-1-dev`.
  Compiler sur la **plus ancienne** Debian visée (glibc). Poste cible : `libxcb-cursor0`.
* macOS : Qt 6.11.x universel, Xcode, CMake, QtIFW 4.11.
* Sans Vulkan SDK : `VULKAN=OFF` avant le script (installeur CPU seulement).
* Windows : la toolchain `mingw1310` de Qt (en-têtes MinGW-w64 11) ne définit pas `THREAD_POWER_THROTTLING_STATE`,
  utilisé par llama.cpp ; `scripts/llama_mingw_compat.ps1` l'ajoute à `ggml-cpu.c` avant la compilation
  (appelé par `build_llama_win.bat`, sans effet avec une toolchain plus récente).

## À faire avant diffusion

* Coller la licence réelle du modèle dans `installer-packages-*/seldonh_model/meta/LICENCE-modele.txt`.
