# SeldonH

Assistant local (Qt/C++ + llama.cpp) d'aide à la datation pour archéologues :
datations simples, **création de modèles ChronoModel** (export `.chr`) et **interprétation des résultats**.
Les calculs de datation s'exécutent toujours sur la machine. Avec le modèle local (llama.cpp), aucune donnée n'est envoyée sur Internet ; avec un modèle distant (API compatible OpenAI, en option), les questions, la conversation, les compétences sélectionnées et les fichiers joints sont envoyés au service choisi.

## Architecture

```
main.cpp, mainwindow.*          fenêtre : dialogue (HTML) + saisie + panneau graphiques
src/llm/LlmCore.*               moteur : enveloppe C++ pure de llama.cpp (sans Qt, testable seule)
src/llm/LlmEngine.*             adaptateur Qt : le moteur tourne dans son propre QThread
src/llm/RemoteApi.*  RemoteEngine.*  modèle distant : API Chat Completions compatible OpenAI (Base URL + clé), réponse en flux SSE
src/core/SecretStore.*          clé API rangée dans le trousseau du système (QtKeychain) ; repli en clair signalé dans les Préférences
src/core/AutoTune.*             « Optimiser pour ce modèle » : contexte, longueur de réponse et budget des compétences calculés
                                d'après le modèle, la mémoire et la vitesse mesurée
src/skills/SkillManager.*       base de compétences (fiches Markdown) + sélection par mots-clés
src/core/ChrBuilder.*           spécification JSON courte  ->  fichier .chr complet et valide
src/core/ConversationStore.*    historique : une conversation = un fichier JSON (tours pour la reprise + journal d'affichage), écriture atomique
src/ui/HistoryPanel.*           volet « Historique » à gauche, repliable (liste, filtre, ouvrir / supprimer / tout effacer)
src/methods/                    méthodes de datation portées de ChronoModel : UN DOSSIER PAR MÉTHODE
  common/                       RefCurve (courbes), Density (grille, HPD, statistiques), Method, registre, MethodRunner,
                                DateFormat (formats de date de ChronoModel : BC/AD, cal BP, B2K, ka, Ma)
  c14/  f14c/  gauss/  unif/  tl/  am/    code de la méthode (+ curves/ et un .qrc quand elle a des courbes)
src/core/ChatText.h             séparation réflexion / réponse (balises <think>)
src/ui/ChatView.*               rendu Markdown -> HTML du dialogue
src/core/MathText.*              formules LaTeX du modèle -> HTML (grec, racines, fractions, exposants, indices) pour le dialogue
src/ui/HelpView.*               mode d'emploi (onglet Aide, F1, /aide) ; source unique : resources/help/aide.html, lisible aussi dans un navigateur
src/ui/FigurePanel.*            courbes (calculs, bloc ```plot), export PNG / SVG ; avec "fill", seule la région HPD (95 % par défaut, clé "hpd") est remplie ;
                                vignettes (thumbnailJson) rappelées dans le dialogue par ChatView::addFigure (lien seldon://figure/<id>)
resources/skills/*.md           compétences intégrées (à enrichir)
tests/                          tests unitaires (tst_core) et outil en ligne de commande (llm_cli)
```

### Fonctionnement d'une question
1. `SkillManager` choisit les fiches pertinentes (mots-clés, sans accents) dans la limite d'un budget de texte ;
   la fiche « identité » (`always: true`) va dans le message système, les autres devant la question.
2. `LlmCore` formate la conversation avec le gabarit du modèle, réutilise le cache KV (seul le nouveau texte est
   évalué), génère en streaming, et oublie les plus anciens échanges si le contexte est plein.
3. L'interface affiche la réponse au fil de l'eau. Deux blocs de code spéciaux sont reconnus :
   - ```` ```chronomodel ```` : spécification de modèle, non affichée telle quelle (`ChrBuilder::displayMarkdown`) : résumé
     « Modèle ChronoModel prêt » + lien « Enregistrer le modèle (.chr) » si `ChrBuilder::build` réussit, sinon liste de ce
     qui manque (événement sans date, nom ou relation inconnus) ; le JSON n'est montré que sur demande explicite ;
   - ```` ```plot ```` : courbe -> onglet du panneau de droite.

### Réponses coupées
Une réponse qui s'arrête avant sa fin naturelle est détectée (`GenStats::truncation`) : limite « Longueur max. d'une
réponse » atteinte (local) ou `finish_reason: "length"` (distant), contexte plein, flux distant interrompu (erreur en
cours de réponse, ou ni `[DONE]` ni `finish_reason`). SeldonH l'écrit sous la réponse et propose **▶ Continuer la
réponse** : le modèle reçoit le début de sa réponse et la consigne de continuer ; la suite s'ajoute au même tour (un bloc
`chronomodel` coupé redevient complet). En mode réflexion, `AppSettings::kThinkingReserve` (2 048 tokens) s'ajoute à la
limite pour que la réflexion ne raccourcisse pas la réponse.

### Calculs faits par l'application, pas par le modèle
Un petit modèle ne sait pas calibrer et invente des règles fausses. Les formules de ChronoModel (plugins 14C, F14C,
Gauss, Unif, TL, AM : vraisemblance, grille au pas de la courbe, aires par trapèzes, troncature 1e-5, HPD par
dichotomie) sont donc reprises en C++ dans `src/methods/`, **un dossier par méthode** :

| dossier | méthode | entrée | courbes embarquées |
|---|---|---|---|
| `c14/` | radiocarbone | âge ± erreur BP, ΔR | IntCal 04/09/13/20, SHCal 04/13/20, Marine 04/09/13/20, UWSY98 |
| `f14c/` | fraction moderne (post-bombe) | F14C ± erreur | Hua 2013 et 2021 (NH zones 1-3, SH) ; les courbes d'âge sont converties |
| `gauss/` | gaussienne, avec ou sans équation g(t) | âge ± erreur | — |
| `unif/` | uniforme | min, max | — |
| `tl/` | TL / OSL | âge ± erreur, année de mesure | — |
| `am/` | archéomagnétisme I, D, F, ID, IF, IDF | inc, déc, α95, F ± erreur | Gallet 2002 (I, D), GwH 2013 (F), Bulgarie |

Deux façons de déclencher un calcul :
1. **La question contient les valeurs** (« C14 de 1954 +- 50 », « TL 800 ± 60 ans, mesuré en 2015 », « uniforme entre
   -500 et -400 »...) : l'application calcule seule, affiche une fiche (HPD 95 % et 68 %, médiane, mode), trace la
   densité dans le panneau de droite et transmet les valeurs exactes au modèle, qui les commente.
2. **Le modèle demande un calcul** : l'application l'exécute puis relance le modèle avec le résultat. Les calculs sont
   définis une seule fois (`src/methods/common/Tools.*` : 6 outils de calcul + `courbes_disponibles`) et exécutés dans
   tous les cas sur l'ordinateur de l'utilisateur, par les mêmes méthodes (`MethodRegistry`) :
   - **modèle distant** : appel d'outils natif de l'API Chat Completions (`tools` / `tool_calls`), jusqu'à
     `RemoteEngine::kMaxToolRounds` (4) allers-retours par question ; si le serveur refuse les outils, repli automatique
     sur le bloc `calcul` ; désactivable dans les Préférences ;
   - **modèle local** : bloc de code de langage `calcul` (JSON, voir la fiche « Outils de calcul intégrés »), un seul
     tour de suite (pas de boucle).
   L'historique est identique dans les deux cas (un appel d'outil est réécrit en bloc `calcul`), donc on peut changer de
   moteur en cours de conversation.

Ajouter une méthode : créer `src/methods/<id>/<Id>Method.{h,cpp}` (classe dérivée de `Method`), ses courbes dans
`curves/` avec un `<id>.qrc`, l'inscrire dans `methods.pri` et dans `MethodRegistry.cpp`.

Ces modules sont dérivés de ChronoModel (CNRS) et distribués, comme tout SeldonH, sous licence GPL-3.0-or-later (voir « Licence »).

Le LLM ne produit **jamais** le `.chr` complet : il remplit un JSON de ~10 champs et `ChrBuilder` ajoute les
200 champs obligatoires (identifiants, UUID, MCMC, plage tmin/tmax, positions…). C'est ce qui rend le résultat
fiable avec un petit modèle.

### Appel d'outils du modèle distant : détection automatique
Préférences → *Appel d'outils* : **Automatique** (défaut), *Toujours*, *Jamais*. En automatique, `ToolsProbe`
(`src/llm/ToolsProbe.*`) envoie au premier usage une requête minimale qui déclare un outil factice (`verifier_outil`)
et classe la réponse : appel natif (`tool_calls`), appel écrit dans le texte, aucun appel, ou champ `tools` refusé. Le
résultat est mémorisé par serveur et par modèle (QSettings `remote/toolsProbe`, refait après 30 jours ; bouton
*Tester*). Les outils ne sont déclarés que si l'appel natif marche ; sinon le bloc ```` ```calcul ```` prend le relais.
Un refus constaté pendant une conversation est mémorisé de même.

### Ordre de deux dates
« Laquelle est la plus ancienne ? » : demande `order` (outil `comparer_dates`) : P(A antérieure à B) calculée exactement
sur les densités (B − A par convolution), densité de l'écart B − A et ses HPD, en supposant les datations
indépendantes. Chaque date peut être une datation, une combinaison ou un événement. Fiche `21_ordre_des_dates`
(imposée par `MethodRunner::asksOrder`) : indépendance, traces ChronoModel (`analyser_traces`, op `order`) et
contraintes stratigraphiques.

### Serveur MCP (`seldonh-mcp`)
`mcp/` : les mêmes outils pour d'autres assistants (Claude Desktop, Jan, Gemini CLI) par le Model Context Protocol,
transport stdio (pas de réseau). Voir `mcp/LISEZMOI.md`.

### Historique des conversations
- Une conversation = un fichier `<identifiant>.json` dans `<dossier de données>/history` (macOS : `~/Library/Application Support/SeldonH/SeldonH/history`).
  Il contient les **tours** (question, texte vu par le modèle, réponse, appels d'outils réécrits en blocs `calcul`) pour **reprendre** le
  dialogue avec n'importe quel moteur, et un **journal d'affichage** (bulles, fiches de calcul, courbes) pour redessiner l'écran.
- Enregistrement automatique après la question, à chaque fin de réponse, en cas d'erreur et à la fermeture ; écriture atomique
  (`QSaveFile`). Désactivable dans les Préférences (« Enregistrer automatiquement les conversations »).
- Rouvrir une conversation enregistre d'abord celle qu'on quitte ; supprimer celle qui est affichée vide l'écran.
- Les fichiers sont en clair et contiennent le texte des pièces jointes : ne rien y mettre de confidentiel si le poste est partagé.

## Compiler

Prérequis : Qt 6, CMake, git, un compilateur C++17 (Xcode sur macOS, MinGW du kit Qt sous Windows) ;
sous Linux, en plus : `pkg-config libsecret-1-dev`.

```bash
./scripts/build_llama.sh                        # clone et compile llama.cpp (Metal sur macOS) -> third_party/llama-install
QTDIR=~/Qt/6.11.1/macos ./scripts/build_qtkeychain.sh   # QtKeychain -> third_party/qtkeychain-install
qmake SeldonH.pro && make                       # ou ouvrir SeldonH.pro dans Qt Creator
```

Windows : `scripts\build_llama_win.bat` et `scripts\build_qtkeychain_win.bat` (variable `QTDIR`, par défaut
`C:\Qt\6.11.2\mingw_64`), puis `qmake` / `mingw32-make`.

**QtKeychain** (0.17.0, licence BSD 2 clauses) range la clé API du modèle distant dans le trousseau du système :
Trousseau macOS (bibliothèque statique universelle), Gestionnaire d'identifiants Windows (`libqt6keychain.dll`),
Secret Service sous Linux (GNOME Keyring, KWallet… ; `libqt6keychain.so`). Compilation vérifiée sous macOS et Windows.
Sans `third_party/qtkeychain-install`, `qmake` affiche un avertissement et SeldonH est construit sans trousseau :
la clé est alors enregistrée en clair dans les réglages, ce que les Préférences indiquent. Les Préférences avertissent
aussi, et demandent confirmation, quand la Base URL est en `http://` vers une autre machine avec une clé.

Installeurs (Windows, Debian/Ubuntu, macOS) : voir `QtInstaller_SeldonH/LISEZMOI_deploiement.md`.

Sur macOS, `build_llama.sh` produit des bibliothèques **universelles** (arm64 + x86_64), comme ChronoModel, pour
fonctionner avec n'importe quel kit Qt. Pour Apple Silicon seulement : `ARCHS=arm64 ./scripts/build_llama.sh`
(alors le kit Qt doit être en arm64, sinon l'éditeur de liens ignore les bibliothèques).

`build_llama.sh` épingle llama.cpp au commit `f849095` (l'API C de `llama.h` évolue vite : le moteur est écrit
contre cette version). Pour une version plus récente : `LLAMA_COMMIT=<commit> ./scripts/build_llama.sh`, puis
corriger d'éventuelles différences dans `LlmCore.cpp`.

## Modèle

Recommandé : **Qwen3.5-4B, quantification Q4_K_M** (≈ 2,7 Go, licence Apache 2.0, contexte natif 262 144 tokens).
GGUF : <https://huggingface.co/unsloth/Qwen3.5-4B-GGUF>

**Où déposer le modèle** : dans le dossier `models/` du projet (menu *Fichier > Ouvrir le dossier des modèles*).
Le modèle est cherché dans cet ordre : réglage « Modèle » des préférences, puis `models/` du projet,
`SeldonH.app/Contents/Resources/models/`, `<dossier de l'exécutable>/models/`, et
`~/Library/Application Support/SeldonH/SeldonH/models/`. Pour **embarquer** le modèle dans l'application :

```bash
macdeployqt SeldonH.app
mkdir -p SeldonH.app/Contents/Resources/models
cp Qwen3.5-4B-Q4_K_M.gguf SeldonH.app/Contents/Resources/models/
```

**Mac Intel** : les GPU AMD/Intel anciens (ex. Radeon Pro 560X : pas de `simdgroup`) ne gèrent pas les noyaux Metal
des modèles récents (texte incohérent). Sur un Mac Intel, `build_llama.sh` compile donc llama.cpp **sans Metal**
(`METAL=ON` pour forcer) et le modèle tourne sur le CPU. En cas de texte incohérent,
`./scripts/build_cli.sh` puis `./build/llm_cli models/<modele>.gguf --cpu` permet de tester le moteur sans interface.

Mode réflexion : désactivé par défaut (réponses plus rapides) ; activable dans les préférences.

## Enrichir les compétences

Une compétence est un fichier `.md` avec un en-tête :

```markdown
---
name: Titre de la fiche
description: Une phrase
keywords: mot, autre mot, expression plus longue
priority: 5          # départage les ex æquo
always: false        # true = toujours dans le message système
---
Contenu en Markdown…
```

Déposez vos fiches dans le dossier des compétences (menu *Compétences > Ouvrir le dossier*), puis
*Recharger les compétences*. Un fichier du même nom qu'une fiche intégrée la remplace.
Conseil : fiches courtes (< 1 500 caractères), un sujet par fiche, des exemples chiffrés vérifiés.

## Tests

```bash
cd tests && qmake tests.pro && make && ./tst_core                      # compétences, ChrBuilder, parsing
c++ ... tests/llm_cli.cpp src/llm/LlmCore.cpp ... -o llm_cli            # voir l'en-tête de tests/llm_cli.cpp
./llm_cli modele.gguf --selftest                                       # moteur : gabarit, cache KV, arrêt, troncature
./llm_cli modele.gguf                                                  # conversation en terminal
```

## Limites connues

- Dates gérées à l'export `.chr` : `gauss`, `unif`, `tl` (plugin `tl/osl`) et `am` (processus I et F vérifiés sur des
  projets réels ; D, ID, IF, IDF suivent l'ordre supposé 2, 4, 5, 6 et déclenchent un avertissement). Les dates 14C et
  `density` ne sont pas générées (la densité référence un fichier CSV de ChronoModel).
- Les valeurs par défaut du `.chr` (échantillonneurs 1 pour les événements et 4 pour les dates, MCMC, courbe) et le
  sens des contraintes (`bwd_id` = événement le plus ancien) ont été relevés dans trois projets ChronoModel 4.0 réels
  (`tests/tst_core.cpp`, test `chrMatchesRealProjects`).
- Un `.chr` généré (exemple de l'aide : deux phases, deux dates gaussiennes, une contrainte) a été importé dans ChronoModel sans
  problème (test de l'utilisateur, 21/10/2026). Les autres types de dates (tl/osl, am) n'ont pas encore été ouverts dans ChronoModel.
- Les calculs intégrés traitent des **dates isolées** (a priori uniforme) ; ils ne remplacent pas ChronoModel pour un
  modèle bayésien complet (phases, contraintes, wiggle matching, combinaison de dates). Non portés : plugin Density
  (courbe tabulée par l'utilisateur), Gauss avec courbe de référence personnelle, courbes AM Mioara, bombe 2004,
  combinaison de dates, étalement (wiggle).
- Âges 14C conventionnels toujours lus en BP. Pour l'archéomagnétisme, la reconnaissance en français est limitée :
  le bloc `calcul` (JSON) est la voie fiable.
- Les HPD sont calculés à 95 % et 68 % (seuils donnés par ChronoModel), sur la grille de la courbe de référence.

## Licence

SeldonH est un logiciel libre : Copyright © CNRS 2026, auteur Philippe DUFRESNE. Il est distribué sous
**GNU General Public License, version 3 ou (à votre choix) toute version ultérieure** (`SPDX-License-Identifier:
GPL-3.0-or-later`), la même licence que ChronoModel, dont il reprend les méthodes de datation (`src/methods`,
Copyright © CNRS 2014 - 2026, auteurs Philippe LANOS, Helori LANOS, Philippe DUFRESNE).
Notice : `LICENSE` ; texte complet : `COPYING` ; chaque fichier source porte l'en-tête de licence.

Composants tiers, chacun sous sa propre licence, compatible avec la GPL v3 :

| composant | licence | usage |
|---|---|---|
| ChronoModel | GPL-3.0-or-later | méthodes de datation, courbes, format `.chr` |
| llama.cpp / ggml | MIT | moteur d'inférence local |
| QtKeychain | BSD 2 clauses | clé API dans le trousseau du système |
| Qt 6 | LGPL v3 (édition open source) | interface ; liaison dynamique |
| Qwen3.5-4B (installeur, facultatif) | Apache 2.0 | modèle de langage, fichier `.gguf` séparé chargé à l'exécution |
