---
name: Outils de calcul intégrés (calcul exact)
description: Demander à SeldonH un calcul exact (calibration 14C, F14C, TL/OSL, Gauss, uniforme, archéomagnétisme) avec un bloc « calcul »
keywords: recuit, annealing, calcul, calculer, event, evenement, combiner, combinaison, mcmc, convergence, rhat, ess, calibrer, calibration, densite, hpd, intervalle, confiance, niveau de confiance, 99 %, 90 %, c14, radiocarbone, f14c, bombe, tl, osl, luminescence, uniforme, gauss, inclinaison, declinaison, intensite, archeomagnetique, archeomagnetisme, datation simple, compute, calculate, calculation, combine, combination, calibrate, density, interval, confidence, confidence level, credible interval, radiocarbon, bomb, uniform, gaussian, normal distribution, inclination, declination, intensity, archaeomagnetic, archaeomagnetism, simple dating, loi normale, log-normale, lognormale, triangulaire, trapezoidale, exponentielle, loi gamma, loi beta, fichier de densite, densite tabulee, creer une densite, plugin density, lognormal, triangular, exponential, density file, tabulated density
priority: 8
---
SeldonH contient les méthodes de calcul de ChronoModel. **Ne calcule jamais toi-même** un intervalle calibré ni une densité : fais-le calculer.

Si la question contient déjà un âge avec son erreur, l'application calcule seule et te transmet un bloc « [Résultat de calcul de SeldonH] » : commente-le. Sinon, quand tu as toutes les valeurs, termine ta réponse par un bloc de code de langage « calcul » (JSON, un objet ou un tableau d'objets) ; l'application calcule, trace la courbe et te renvoie les valeurs exactes :

```calcul
{"method":"c14","age":2500,"error":30,"curve":"intcal20"}
```

**Si des fonctions (outils) te sont fournies** (modèle distant), appelle-les au lieu d'écrire le bloc (mêmes paramètres) ; ne rappelle pas un outil dont le résultat est déjà dans le message.

Méthodes (clé « method ») : `c14`, `f14c`, `tl`, `gauss`, `unif`, `am` — leurs champs sont dans les fiches « Date — … » (les mêmes que dans un modèle ChronoModel, avec `"method"` au lieu de `"type"`) ; n'emploie aucun autre nom de champ.
- `event` : événement de ChronoModel (MCMC) combinant plusieurs datations : `{"method":"event","name":"US 12","dates":[{"method":"c14","age":2500,"error":30},{"method":"tl","age":2600,"error":90,"ref_year":2015}]}` ; facultatifs `tmin`, `tmax`, `mcmc` (seulement si l'utilisateur les donne ; « sans recuit » → `{"anneal_recurrence":0}`).
- `combine` (combinaison, produit des vraisemblances : « combine ») ; `traces` (durées, dates sur les traces MCMC jointes) ; `inverse` (mesure attendue pour une date) : voir la fiche « Combinaison, traces MCMC et calibration inverse ».
- `density_file` : fichier CSV pour le plugin Density de ChronoModel, à partir d'une loi classique (outil `creer_densite`). `law` : normal (mean, sd), lognormal (meanlog, sdlog, ou median et factor), uniform (min, max), triangular (min, mode, max), trapezoidal (min, mode_min, mode_max, max), exponential (mean ou rate), gamma (shape et scale, ou mean et sd), beta (alpha, beta, sur [min ; max]). Placement sur l'axe du temps : t = offset + x, ou t = offset − x avec `"reverse": true` (durée comptée vers le passé). Facultatifs : `tmin`, `tmax`, `step` ou `points`, `name`. Ex. durée log-normale avant 1200 : `{"method":"density_file","law":"lognormal","median":30,"factor":2,"offset":1200,"reverse":true}`. L'utilisateur enregistre le fichier avec le lien affiché ; ne recopie pas les valeurs.
- Facultatifs pour toutes les datations : `wiggle` (décalage δ donné par l'utilisateur ; résultat = t + δ) `{"type":"fixed","value":20}`, `{"type":"range","min":10,"max":30}` ou `{"type":"gaussian","mean":20,"error":5}` ; `name` (repris dans le titre du graphique ; pour une date .chr, reprends son `name`) ; `color` (seulement si donnée) ; `level` (niveau HPD demandé, ex. 99) ; `tmin`, `tmax` (période d'étude en années calendaires signées, seulement si l'utilisateur la donne : la densité est calculée sur cette période et renormalisée ; sans eux, toute la courbe) ; `"layout":"overlay"` seulement si l'utilisateur veut les densités superposées (sinon une par ligne).
- Une date copiée d'un fichier .chr (`plugin_id`, `data`, `delta_type`…) peut être mise telle quelle : l'application la convertit.

« Refais la datation à 50 % / entre 0 et 2000 » : SeldonH recalcule seul la datation précédente ; commente le bloc de résultat, ne réécris pas de bloc « calcul » (tu perdrais les courbes choisies). N'ajoute jamais de clé qui n'existe pas dans cette liste.

Il manque une valeur (erreur, année de mesure, α95…) : pose la question, n'invente rien.
