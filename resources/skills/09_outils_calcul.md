---
name: Outils de calcul intégrés (calcul exact)
description: Demander à SeldonH un calcul exact (calibration 14C, F14C, TL/OSL, Gauss, uniforme, archéomagnétisme) avec un bloc « calcul »
keywords: calcul, calculer, event, evenement, combiner, combinaison, mcmc, convergence, rhat, ess, calibrer, calibration, densite, hpd, intervalle, confiance, niveau de confiance, 99 %, 90 %, c14, radiocarbone, f14c, bombe, tl, osl, luminescence, uniforme, gauss, inclinaison, declinaison, intensite, archeomagnetique, archeomagnetisme, datation simple
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
- `wiggle` (facultatif, toute datation simple, seulement si l'utilisateur donne un décalage δ entre l'échantillon et ce qu'il veut dater) : `{"type":"fixed","value":20}`, `{"type":"range","min":10,"max":30}` ou `{"type":"gaussian","mean":20,"error":5}` ; le résultat est la densité de t + δ.
- Les dates peuvent aussi être copiées telles quelles d'un fichier .chr (`plugin_id`, `data`, `delta_type`…) : l'application les convertit (méthode, courbes, wiggle). Ne recopie pas les champs à la main.
- `event` : **événement** (Event de ChronoModel) estimé par MCMC à partir de plusieurs datations du même événement, de méthodes éventuellement différentes : `{"method":"event","name":"US 12","dates":[{"method":"c14","age":2500,"error":30},{"method":"tl","age":2600,"error":90,"refYear":2015}]}`. Chaque date peut avoir un décalage `wiggle` : `{"type":"fixed","value":20}`, `{"type":"range","min":10,"max":30}` ou `{"type":"gaussian","mean":20,"error":5}`. Facultatifs : `tmin`, `tmax` (période d'étude), `mcmc` (`num_iter`, `num_burn`, `num_proc` = chaînes, `seeds`…) — ne les mets que si l'utilisateur les donne : sinon ce sont ceux de ChronoModel (4 chaînes, burn-in 1000, 100 000 itérations, thinning 10). Outil : `calculer_evenement`.
- Toutes les méthodes : `level` (facultatif) = niveau de confiance des intervalles HPD en %, 95 par défaut. Ne le mets que si l'utilisateur demande un autre niveau (« à 99 % », « niveau de confiance 90 ») ; les HPD à 95 % et 68 % sont toujours données en plus. Ex. `{"method":"c14","age":2500,"error":30,"level":99}`.

Il manque une valeur (erreur, année de mesure, α95…) : pose la question, n'invente rien.
