---
name: Conventions de datation et calculs simples
description: BP, BC/AD, année zéro, moyenne pondérée, test de cohérence de dates
keywords: bp, calendaire, avant jc, av. j.-c., ad, bc, annee zero, moyenne ponderee, ecart-type, sigma, coherence, ward et wilson, khi2, chi2, combiner, combinaison, conversion, convertir, age calendaire
priority: 5
---
**Échelles**
- BP (Before Present) : présent = 1950. Année calendaire AD = 1950 − BP. Exemple : 2500 BP = −550 (550 av. J.-C. en numérotation astronomique).
- Numérotation : en numérotation astronomique l'année 0 existe (1 av. J.-C. = 0, 2 av. J.-C. = −1). En numérotation historique il n'y a pas d'année 0. Toujours préciser laquelle est utilisée (ChronoModel travaille avec des années signées).
- Âge 14C conventionnel (BP) ≠ âge calendaire : il doit être calibré.

**Erreur de mesure** : une erreur « ±σ » est un écart-type (1σ, ≈ 68 %) d'une loi gaussienne ; ±2σ ≈ 95 %. Si un laboratoire donne ±2σ, divise par 2 avant d'utiliser le σ. Cela ne vaut **que pour la mesure** : une densité calendaire (calibrée ou a posteriori) n'est pas gaussienne, on la résume par des HPD à 95 % et 68 %, jamais par des « sigma ».

**Moyenne pondérée de n dates gaussiennes (a_i ± s_i)** (mesures indépendantes du même événement) :
- poids w_i = 1 / s_i²
- moyenne = Σ(w_i·a_i) / Σ w_i
- erreur de la moyenne = 1 / √(Σ w_i)

**Test de cohérence (Ward et Wilson)** : T = Σ w_i·(a_i − moyenne)². Sous l'hypothèse « même événement », T suit un χ² à n−1 degrés de liberté. Seuils à 5 % : n=2 → 3,84 ; n=3 → 5,99 ; n=4 → 7,81 ; n=5 → 9,49 ; n=6 → 11,07. Si T dépasse le seuil, les dates ne sont pas statistiquement compatibles : ne pas les combiner sans examiner le contexte.
