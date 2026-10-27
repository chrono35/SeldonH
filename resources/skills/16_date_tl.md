---
name: Date — luminescence (TL/OSL)
description: Champs d'une date TL/OSL (âge avant l'année de mesure), pour un modèle ou un calcul
keywords: tl, osl, irsl, luminescence, thermoluminescence, optically stimulated, annee de mesure, fait en, mesure en
priority: 4
---
Âge en années AVANT l'année de la mesure, erreur 1σ.
- Modèle : `{"type":"tl","name":"<référence>","age":<âge>,"error":<erreur>,"ref_year":<année de la mesure>}`
- Calcul : `{"method":"tl","age":<âge>,"error":<erreur>,"ref_year":<année de la mesure>}`
- `ref_year` est OBLIGATOIRE : « fait en 2010 », « mesuré en 2010 » → `"ref_year":2010`. S'il n'est pas donné, demande-le ; ne mets jamais l'année en cours. Un âge en ka se convertit en années (12,5 ka → 12500).
Remplace chaque <…> par la valeur donnée par l'utilisateur et omets un champ facultatif qu'il n'a pas donné. Dans un modèle ChronoModel, la date s'écrit avec `"type"` ; dans un bloc « calcul » ou un outil, avec `"method"` : mêmes champs.
