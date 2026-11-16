---
name: Créer une densité tabulée
description: Écrire un fichier CSV de densité pour le plugin Density de ChronoModel à partir d'une loi classique (calcul « density_file »)
keywords: loi normale, log-normale, lognormale, triangulaire, trapezoidale, exponentielle, loi gamma, loi beta, fichier de densite, creer une densite, creer un fichier de densite, lognormal, triangular, trapezoidal, exponential, gamma distribution, beta distribution, density file
priority: 6
---
`density_file` : fichier CSV pour le plugin Density de ChronoModel, à partir d'une loi classique (outil `creer_densite`). `law` : normal (mean, sd), lognormal (meanlog, sdlog, ou median et factor), uniform (min, max), triangular (min, mode, max), trapezoidal (min, mode_min, mode_max, max), exponential (mean ou rate), gamma (shape et scale, ou mean et sd), beta (alpha, beta, sur [min ; max]). Placement sur l'axe du temps : t = offset + x, ou t = offset − x avec `"reverse": true` (durée comptée vers le passé). Facultatifs : `tmin`, `tmax`, `step` ou `points`, `name`. Ex. durée log-normale avant 1200 : `{"method":"density_file","law":"lognormal","median":30,"factor":2,"offset":1200,"reverse":true}`. L'utilisateur enregistre le fichier avec le lien affiché ; ne recopie pas les valeurs.
Demande : bloc « calcul » ou outil `creer_densite`. Paramètres de la loi : ceux de l'utilisateur seulement ; s'il en manque un, pose la question.
