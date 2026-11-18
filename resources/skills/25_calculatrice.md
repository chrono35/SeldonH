---
name: Calculatrice
description: Tout calcul numérique (racine, puissance, moyenne, différence d'années, conversion BP ↔ année, degrés-minutes-secondes ↔ degrés décimaux, degrés ↔ radians) par la calculatrice de SeldonH
keywords: calculatrice, calculator, racine, racine carree, racine cubique, square root, sqrt, puissance, power, carre, au carre, logarithme, exponentielle, formule, moyenne, ecart-type, ecart type, somme, pourcentage, arithmetique, multiplie, divise, soustraction, difference, convertir, conversion, degres decimaux, decimal degrees, degres minutes secondes, dms, sexagesimal, coordonnees, latitude, longitude, radian, radians, rad
priority: 6
---
**Aucun nombre calculé de tête**, même simple (√36, 2^10, moyenne de trois âges, écart entre deux années) : SeldonH le calcule.
```calcul
{"method":"expr","expr":"sqrt(36)"}
```
Outil `calculatrice` (mêmes paramètres) si des fonctions te sont fournies. Recopie les nombres de l'utilisateur dans la formule. Opérateurs `+ - * / ^`, parenthèses ; `sqrt`, `exp`, `ln`, `log10`, `abs`, `round(x; n)`, `min`, `max`, `sin`/`cos`/`tan` (degrés) ; `moyenne(a; b; c)`, `ecart_type(…)`, `moy_ponderee(…)` ; `ADtoBP`, `BPtoAD`, `C14toF14C(âge; erreur)` ; mesure avec erreur `2500 +- 30` (propagée, erreurs indépendantes). Angles : un angle écrit `W 00° 05' 52''` ou `48° 06′ 51″ N` est compris tel quel (S, W, O négatifs) ; `dms(d; m; s)` → degrés décimaux, `DDtoDMS(x)` → degrés, minutes, secondes ; `rad(x)` degrés → radians, `deg(x)` radians → degrés (`pi` ou `π` connus). Arguments séparés par « ; ».
Cite le résultat de SeldonH à l'identique.
