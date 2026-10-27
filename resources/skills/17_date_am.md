---
name: Date — archéomagnétisme
description: Champs d'une date archéomagnétique (inclinaison, déclinaison, intensité), pour un modèle ou un calcul
keywords: archeomagnetisme, archeomagnetique, inclinaison, declinaison, intensite, alpha95, paleointensite, archaeomagnetic, archaeomagnetism, inclination, declination, intensity
priority: 4
---
`process` = grandeurs mesurées : I (inclinaison), D (déclinaison), F (intensité), ID, IF ou IDF.
- I : `"inc":<inclinaison>,"alpha95":<α95>` ; D : en plus `"dec":<déclinaison>` (D demande aussi inc et alpha95) ; F : `"field":<intensité µT>,"error_f":<erreur µT>`.
- Modèle (inclinaison seule) : `{"type":"am","name":"<référence>","process":"I","inc":<inclinaison>,"alpha95":<α95>}`
- Calcul : `{"method":"am","process":"I","inc":<inclinaison>,"alpha95":<α95>}`
- N'écris que les grandeurs données : pas de `field`/`error_f` sans intensité, pas de `dec` sans déclinaison.
- Le mot qui suit « am » (« am Rennes2025 ») est la référence (`name`), pas une courbe. Courbes `curve_i`, `curve_d`, `curve_f` seulement si l'utilisateur écrit le mot « courbe » (défaut : Paris, Gallet 2002).
Remplace chaque <…> par la valeur donnée par l'utilisateur et omets un champ facultatif qu'il n'a pas donné. Dans un modèle ChronoModel, la date s'écrit avec `"type"` ; dans un bloc « calcul » ou un outil, avec `"method"` : mêmes champs.
