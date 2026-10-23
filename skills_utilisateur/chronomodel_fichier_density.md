---
name: Créer un fichier de densité pour le plugin Density de ChronoModel
description: Format exact du fichier CSV lu par le plugin Density (densité tabulée), pièges et exemples
keywords: densite tabulee, densites tabulees, plugin density, fichier density, fichier de densite, density, calib/density, densite a priori, loi a priori tabulee, histogramme, courbe de densite, csv density, tabulated density, density file, density plugin, prior density, histogram
priority: 6
---
Le plugin Density de ChronoModel lit une densité de probabilité en années calendaires (signées : négatif = avant J.-C.) dans un fichier CSV. Format exact (PluginDensity::loadRefFile) :

1. Une valeur par ligne ; seule la 1re colonne est lue (séparateur virgule, point décimal, format anglais). Les autres colonnes sont ignorées.
2. Lignes de commentaire : commencent par `//`, `#`, `/*` ou `!`. Une ligne non numérique est aussi sautée, mais préfère `//`.
3. 1re valeur = tmin, 2e valeur = tmax (permutées si tmin > tmax).
4. Puis N valeurs de densité (N ≥ 2) sur une grille RÉGULIÈRE : t_k = tmin + k·(tmax − tmin)/(N − 1), k = 0…N−1. La 1re valeur est en tmin, la dernière en tmax.
5. Valeurs ≥ 0, pas besoin de normaliser : ChronoModel ramène l'aire à 1. Interpolation linéaire entre les points ; densité nulle hors de [tmin ; tmax].

Piège principal : un fichier à deux colonnes « année, densité » est FAUX (la colonne des années serait lue comme densité). Si l'utilisateur a des couples (t, valeur) irréguliers, interpole-les sur une grille régulière, puis écris tmin, tmax et la seule colonne des valeurs.

Exemples (dossier Calib/Density de ChronoModel) :
```csv
// gate [20; 40] : uniforme entre 20 et 40
20
40
0.05
0.05
```
```csv
// dnorm(seq(-10, 10, length.out = 21), mean = 0, sd = 2)
-10
10
7.433598e-07
… (21 valeurs au total)
```

Installation : nom libre avec l'extension `.csv` (ChronoModel compare les noms en minuscules). Copier le fichier dans le dossier Calib/Density de ChronoModel : macOS `~/Library/Application Support/CNRS/ChronoModel/Calib/Density`, Windows `C:\Users\<nom>\AppData\Local\CNRS\ChronoModel\Calib\Density`, Linux `~/.local/share/CNRS/ChronoModel/Calib/Density`. Relancer ChronoModel. Dans un modèle SeldonH : `{"type":"density","name":"D-1","curve":"nom_du_fichier.csv"}`.

Comment répondre :
- Loi classique (normale, log-normale, uniforme, triangulaire, trapézoïdale, exponentielle, gamma, bêta) : fais créer le fichier par SeldonH avec un bloc « calcul » `{"method":"density_file","law":…}` (ou l'outil `creer_densite`). Placement sur l'axe du temps : `offset` (t = offset + x) et `"reverse": true` pour une durée comptée vers le passé (t = offset − x). Ex. « au plus quelques décennies avant 1200, médiane 30 ans » : `{"method":"density_file","law":"lognormal","median":30,"factor":2,"offset":1200,"reverse":true}`. SeldonH trace la densité et affiche un lien « Enregistrer le fichier… » qui propose directement le dossier Calib/Density.
- Autre forme (tableau de points, histogramme) : donne le fichier complet dans un bloc de code « csv » (1re ligne de commentaire, tmin, tmax, valeurs) si les valeurs sont données ; sinon un script R ou Python qui l'écrit. N'invente aucune valeur.
- Pas de la grille : au plus le pas de la période d'étude du modèle.
