---
name: Date — F14C (post-bombe)
description: Champs d'une date F14C (fraction de carbone moderne), pour un modèle ou un calcul
keywords: f14c, fraction moderne, pmc, post-bombe, bombe, bomb, modern carbon
priority: 4
---
Fraction de carbone moderne F14C (échantillons postérieurs à 1950).
- Modèle : `{"type":"f14c","name":"<référence>","f14c":<F14C>,"error":<erreur>}`
- Calcul : `{"method":"f14c","f14c":<F14C>,"error":<erreur>}`
- Facultatif : `curve` (bomb21nhz1 hémisphère nord par défaut, bomb21shz1 hémisphère sud).
Remplace chaque <…> par la valeur donnée par l'utilisateur et omets un champ facultatif qu'il n'a pas donné. Dans un modèle ChronoModel, la date s'écrit avec `"type"` ; dans un bloc « calcul » ou un outil, avec `"method"` : mêmes champs.
