---
name: Outils de calcul intégrés (calcul exact)
description: Demander à SeldonH un calcul exact (calibration 14C, F14C, TL/OSL, Gauss, uniforme, archéomagnétisme) avec un bloc « calcul »
keywords: calcul, calculer, calibrer, calibration, densite, hpd, intervalle, c14, radiocarbone, f14c, bombe, tl, osl, luminescence, uniforme, gauss, inclinaison, declinaison, intensite, archeomagnetique, archeomagnetisme, datation simple
priority: 8
---
SeldonH contient les méthodes de calcul de ChronoModel (densités calendaires, intervalles HPD). **Ne calcule jamais toi-même** un intervalle calibré : fais-le calculer.

Si la question contient déjà un âge avec son erreur, l'application calcule seule et te transmet un bloc « [Résultat de calcul de SeldonH] » : commente-le. Sinon, quand tu as toutes les valeurs, termine ta réponse par un bloc de code de langage « calcul » (JSON, un objet ou un tableau d'objets) ; l'application le calcule, trace la courbe et te renvoie les valeurs exactes pour que tu les expliques :

```calcul
{"method":"c14","age":2500,"error":30,"curve":"intcal20"}
```

**Si des fonctions (outils) te sont fournies dans la conversation** (modèle distant), appelle-les directement au lieu d'écrire le bloc : `calibrer_radiocarbone` (c14), `calibrer_f14c`, `dater_luminescence` (tl), `densite_gauss`, `densite_uniforme`, `dater_archeomagnetisme` (am) et `courbes_disponibles`. Les paramètres sont les mêmes que ci-dessous. Ne rappelle pas un outil quand le résultat est déjà fourni dans le message.

Méthodes (clé « method ») :
- `c14` : age (BP), error, curve (intcal20, intcal13, shcal20, marine20…), deltaR et deltaRerr si marin.
- `f14c` : f14c, error, curve (bomb21nhz1 post-bombe nord, bomb21shz1 sud, intcal20 avant la bombe).
- `tl` : age et error (années avant l'année de mesure), refYear (année de mesure).
- `gauss` : age et error en années calendaires signées (négatif = avant J.-C.).
- `unif` : min et max (années calendaires signées).
- `am` : process (I, D, F, ID, IF ou IDF), inc, dec, alpha95, field et errorF (µT).

Il manque une valeur (erreur, année de mesure, α95…) : pose la question, n'invente rien.
