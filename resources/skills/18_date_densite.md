---
name: Date — densité tabulée
description: Date donnée par un fichier de densité (plugin Density de ChronoModel)
keywords: densite tabulee, fichier de densite, plugin density, density file, tabulated density
priority: 4
---
Densité lue dans un fichier CSV du dossier Calib/Density de ChronoModel.
- Modèle : `{"type":"density","name":"<référence>","curve":"<fichier>.csv"}` ; le nom du fichier est celui de l'utilisateur, ne l'invente pas.
- Pour CRÉER un tel fichier à partir d'une loi (normale, log-normale…), voir la fiche « Outils de calcul intégrés » (`density_file`).
Remplace chaque <…> par la valeur donnée par l'utilisateur et omets un champ facultatif qu'il n'a pas donné. Dans un modèle ChronoModel, la date s'écrit avec `"type"` ; dans un bloc « calcul » ou un outil, avec `"method"` : mêmes champs.
