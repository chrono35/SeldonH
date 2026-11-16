---
name: Modèle — courbe
description: Modèle ChronoModel en mode courbe (archéomagnétisme I, D, F ou grandeurs X, Y, Z) : mesure portée par chaque Event
keywords: courbe vectorielle, courbe spherique, variation seculaire, secular variation, courbe archeomagnetique, mode courbe, curve mode, univariee, bivariee, trivariee, univariate, bivariate, trivariate
priority: 40
---
`"curve"` à la racine : `"vector"` (I, D, F), `"spherical"` (I, D), `"unknown_dec"` (I, F), `"inclination"`, `"declination"`, `"intensity"` ; grandeurs quelconques : `"univariate"` (X), `"bivariate"` (X, Y), `"trivariate"` (X, Y, Z). Âge-profondeur : fiche à part.
- Chaque Event (borne comprise) porte SA mesure sur l'événement, jamais dans `dates` : archéomagnétisme `"inc"`, `"dec"`, `"alpha95"` (degrés), `"field"`, `"error_f"` (µT) ; sinon `"X_Inc_Depth"`, `"sX_Alpha95_sDepth"`, `"Y_Dec"`, `"sY"`, `"Z_Field"`, `"sZ_sF"` (valeur et erreur de X, Y, Z).
- Ses `dates` restent ses datations (c14, unif, gauss…) : jamais une date « am » pour une mesure de la courbe.
- Quand SeldonH l'annonce (« SeldonH recopie les mesures »), n'écris que les Events, leurs dates et les contraintes : les mesures sont ajoutées par SeldonH, avec le nom de l'Event écrit comme dans le texte.
