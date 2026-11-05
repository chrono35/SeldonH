# seldonh-mcp — les calculs de SeldonH dans d'autres assistants (MCP)

`seldonh-mcp` met les **outils de calcul de SeldonH** (méthodes de ChronoModel) à disposition d'autres assistants
par le **Model Context Protocol** (MCP), transport **stdio** : l'assistant lance le programme sur l'ordinateur et lui
parle en JSON-RPC, un message par ligne, sur l'entrée et la sortie standard.

- Pas de réseau, pas de port ouvert, pas de service permanent : le programme démarre avec l'assistant et s'arrête avec lui.
- Les calculs sont **les mêmes que dans SeldonH** (`src/methods/common/Tools.*`, `MethodRunner`), faits sur cet ordinateur.
- Les réglages de SeldonH sont relus : dossier des courbes de l'utilisateur, courbes de ChronoModel (dossier `Calib`),
  format des dates et décimales. `--date-format calbp` (ou `ka`, `bcce`…) remplace le format.
- QtCore seulement (ni interface, ni réseau, ni llama.cpp).

## Outils

Les 15 outils de SeldonH : `calibrer_radiocarbone`, `calibrer_f14c`, `dater_luminescence`, `densite_gauss`,
`densite_uniforme`, `dater_archeomagnetisme`, `calculer_evenement` (MCMC, avec notifications de progression),
`combiner_dates`, `courbe_chronomodel` (courbe estimée par ChronoModel : valeur, vitesse, accélération, date d'atteinte d'une valeur, date d'une mesure sur la courbe ou sur sa vitesse, tracé des courbes de vitesse et d'accélération dans `extra_plots`), `comparer_dates` (ordre, durée, date entre, appartenance ou corrélation de dates ; la corrélation renvoie aussi les nuages de points dans `extra_plots`), `analyser_traces`, `tableau`
(tableau de datations : sélection, extraction, datation, événement, modèle),
`calibration_inverse`, `creer_densite`, `courbes_disponibles`. `seldonh-mcp --list` les affiche.

Différences avec l'application :
- `tableau` + `file` : chemin d'un tableau CSV, TSV ou texte (lu et gardé). Les .xlsx et .ods sont lus par l'application,
  pas par ce serveur (QtCore seul) : enregistrez-les d'abord en CSV. Les lignes extraites et le modèle préparé sont rendus dans le texte.
- `analyser_traces` : argument supplémentaire `file`, chemin du fichier de traces exporté par ChronoModel (aussi accepté par `comparer_dates` quand un terme est une colonne de traces)
  (`Chain_all_Events.csv`…), lu sur le disque ; il reste chargé pour les appels suivants.
- `courbe_chronomodel` : arguments supplémentaires `file` (trace des splines `Curve_X_Splines.csv`, ou carte de densité
  `Curve_X_Map.csv`) et `traces_file` (facultatif : `Chain_all_Events.csv` du même calcul, pour les chaînes et les noms
  des Events) ; lus sur le disque et gardés pour les appels suivants. Les graphiques sont dans `extra_plots`.
- `creer_densite` : argument supplémentaire `dossier` ; le fichier CSV y est écrit (par exemple le dossier
  `Calib/Density` de ChronoModel). Sans `dossier`, rien n'est écrit.
- Pas de panneau de graphiques : le texte du résultat le dit, et les densités tabulées (x, y) sont dans
  `structuredContent.plot` (et `extra_plots` pour un écart, une durée ou une calibration inverse).

## Compiler

```bash
qmake mcp/seldonh-mcp.pro && make          # macOS / Linux ; Windows : mingw32-make
./seldonh-mcp --list                         # vérification
```

Les installeurs (`QtInstaller_SeldonH`) le compilent et l'installent avec SeldonH :
`C:\Program Files\SeldonH\seldonh-mcp.exe`, `~/SeldonH/seldonh-mcp` (Linux),
`/Applications/SeldonH/SeldonH.app/Contents/MacOS/seldonh-mcp` (macOS). Le fichier `LISEZMOI_MCP.txt` du dossier
d'installation donne le chemin exact et la configuration à copier.

## Brancher sur un assistant

Remplacer le chemin par celui de `seldonh-mcp` (chemin absolu).

**Claude Desktop** — *Réglages > Développeur > Modifier la configuration* (`claude_desktop_config.json` :
`~/Library/Application Support/Claude/` sur macOS, `%APPDATA%\Claude\` sous Windows), puis relancer Claude Desktop :

```json
{
  "mcpServers": {
    "seldonh": { "command": "/Applications/SeldonH/seldonh-mcp", "args": [] }
  }
}
```

**Jan** — *Settings > MCP Servers > + Add MCP Server* : nom `seldonh`, commande = chemin de `seldonh-mcp`, sans
argument ; activer le serveur. Le modèle choisi doit savoir faire de l'appel d'outils (option à activer dans les
paramètres du modèle pour un modèle local).

**Gemini CLI** (ou Antigravity CLI) — `gemini mcp add seldonh /chemin/vers/seldonh-mcp`, ou dans `~/.gemini/settings.json` :

```json
{ "mcpServers": { "seldonh": { "command": "/chemin/vers/seldonh-mcp", "args": [] } } }
```

ChatGPT (mode développeur) et l'application web de Gemini n'acceptent que des serveurs MCP **distants** (HTTPS) :
`seldonh-mcp` ne s'y branche pas tel quel. Un transport HTTP pourrait être ajouté (mêmes messages JSON-RPC), mais les
calculs se feraient alors sur le serveur qui l'héberge.

## Diagnostic : l'assistant appelle-t-il les outils ?

Si l'assistant répond par des dates inventées, il n'a pas appelé l'outil (le serveur renvoie toujours les valeurs
exactes). Le journal des échanges le montre : ajouter `--log <fichier>` aux arguments (ou la variable d'environnement
`SELDONH_MCP_LOG`), par exemple dans Jan : `"args": ["--log", "/tmp/seldonh-mcp.log"]`.

```
… --- seldonh-mcp 1.0.0 démarré …
… --> {"method":"initialize" …}            l'assistant s'est connecté
… --> {"method":"tools/list" …}            il a lu la liste des outils
… --> {"method":"tools/call","params":{"name":"calibrer_radiocarbone","arguments":{"age":3548,"error":25}}}
… <-- {"result":{"content":[{"text":"[Résultat de calcul de SeldonH …"
```

Pas de `tools/list` : l'assistant n'utilise pas ce serveur. `tools/list` sans `tools/call` : le modèle ne reçoit pas
les outils ou choisit de ne pas les appeler (capacité « Tools » du modèle dans l'assistant, modèle trop petit).
`tools/call` présent : le calcul est fait ; si la réponse affichée diffère, c'est le modèle qui l'a mal recopiée.

## Essai à la main

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"essai","version":"1"}}}' \
              '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
              '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"comparer_dates","arguments":{"a":{"method":"c14","age":2500,"error":30},"b":{"method":"c14","age":2450,"error":30}}}}' \
  | ./seldonh-mcp
```

Tests : `tests/tst_core` (`mcpServer`). Vérifié aussi avec le client MCP officiel (SDK Python `mcp` 2.3) :
initialisation (protocole 2025-11-25), liste des outils, appels, progression d'un Event, erreurs.
