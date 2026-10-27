---
name: Date — radiocarbone (14C)
description: Champs d'une date 14C (âge conventionnel BP), pour un modèle ou un calcul
keywords: c14, 14c, radiocarbone, carbone 14, bp, intcal, intcal20, shcal20, marine20, delta r, reservoir, radiocarbon, carbon 14
priority: 4
---
Âge radiocarbone conventionnel NON calibré, en BP (avant 1950) ; l'application calibre.
- Modèle : `{"type":"c14","name":"<référence>","age":<âge BP>,"error":<erreur>}`
- Calcul : `{"method":"c14","age":<âge BP>,"error":<erreur>}`
- Facultatifs : `curve` (intcal20 par défaut ; shcal20 pour l'hémisphère sud, marine20 pour un échantillon marin) ; `delta_r` et `delta_r_error` (échantillon marin seulement, s'ils sont donnés).
- « 14c 6589 +- 30 » → `"age":6589,"error":30`.
Remplace chaque <…> par la valeur donnée par l'utilisateur et omets un champ facultatif qu'il n'a pas donné. Dans un modèle ChronoModel, la date s'écrit avec `"type"` ; dans un bloc « calcul » ou un outil, avec `"method"` : mêmes champs.
