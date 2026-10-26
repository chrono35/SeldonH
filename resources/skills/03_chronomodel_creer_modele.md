---
name: Créer un modèle ChronoModel
description: Format du bloc chronomodel (JSON simplifié) pour générer un fichier .chr
keywords: recuit, annealing, wiggle, decalage, delta, wiggle-matching, modele, modeles, creer un modele, construire un modele, faire un modele, fais moi un modele, fichier chr, .chr, exporter, generer, modeliser, modelisation, evenement, evenements, phase, phases, tableau de dates, importer dans chronomodel, projet chronomodel, contrainte, contraintes, stratigraphie, plus ancien, plus ancienne, offset, model, models, create a model, build a model, make a model, chr file, export, generate, modelling, modeling, event, events, table of dates, import into chronomodel, chronomodel project, constraint, constraints, stratigraphy, older, younger, anterieur, posterieur, au dessus, en dessous, sous jacent, precede, succede, recoupe, recouvre, overlies, underlies, older than, younger than
priority: 50
---
Pour créer un modèle, ÉCRIS un bloc de code de langage « chronomodel » (JSON valide, sans commentaire), exactement dans ce format ; l'application complète le fichier .chr.

- `name` : nom du projet. `phases` : `[{"name":"Phase 1"}]` (facultatif).
- `events` (obligatoire) : `{"name":"US 12","phases":["Phase 1"],"dates":[…]}` (nom unique) ; borne : `{"name":"B","type":"bound","known_fixed":-700}`.
- Dates (`name` facultatif sur chacune) :
  - gauss : `{"type":"gauss","age":-520,"error":35}` (année calendaire signée, error = 1σ > 0)
  - unif : `{"type":"unif","min":-600,"max":-450}`
  - c14 : `{"type":"c14","age":2500,"error":30,"curve":"intcal20"}` ; âge BP NON calibré ; `curve` facultative (shcal20 sud, marine20 marin) ; `delta_r`, `delta_r_error` si marin et donnés.
  - f14c : `{"type":"f14c","f14c":1.2,"error":0.01,"curve":"bomb21nhz1"}`.
  - tl : `{"type":"tl","age":488,"error":19,"ref_year":2025}` ; âge en années AVANT ref_year (année de mesure).
  - am : `{"type":"am","process":"IF","inc":65,"alpha95":1,"field":54.2,"error_f":0.8}` ; process I, D, F, ID, IF ou IDF ; I : inc + alpha95 ; D : dec + inc + alpha95 ; F : field (µT) + error_f ; courbes facultatives `curve_i`, `curve_d`, `curve_f`.
  - density : `{"type":"density","curve":"fichier.csv"}` ; nom de fichier donné par l'utilisateur (ne l'invente pas), fichier dans Calib/Density de ChronoModel.
- `wiggle` (facultatif, toute date, seulement si l'utilisateur donne un décalage δ ; l'événement est daté par t + δ, garde le signe donné) : `{"type":"fixed","value":-10}`, `{"type":"range","min":-10,"max":10}` ou `{"type":"gaussian","mean":1000,"error":200}`.
- `event_constraints` (entre événements) et `phase_constraints` (entre phases) : `["A", "<relation>", "B"]` en RECOPIANT le mot de l'utilisateur, l'application en déduit le sens (français ou anglais) : avant / après, antérieur / postérieur, plus ancien / plus récent, précède / succède, en dessous / au-dessus, sous / sur, coupe, recoupe, recouvre, scelle, est coupé par, before / after, below / above, cuts, is cut by… Ex. `["US 10", "au-dessus de", "US 12"]`. Une séquence `["US 12", "US 11", "US 10"]` va du plus ancien au plus récent. Relation sans ordre clair (« contemporain ») : pose la question.
- `settings` (facultatif) : `tmin`, `tmax`, `step`. `mcmc` (seulement ce que l'utilisateur donne) : `num_iter`, `num_burn`, `num_proc`, `thinning_interval`, `seeds`… ; « pas de recuit » → `"mcmc": {"anneal_recurrence": 0}`.

Exemple :
```chronomodel
{
  "name": "Site test",
  "phases": [{"name": "Phase ancienne"}, {"name": "Phase récente"}],
  "events": [
    {"name": "US 12", "phases": ["Phase ancienne"],
     "dates": [{"type": "gauss", "name": "Ech-1", "age": -520, "error": 35}]},
    {"name": "US 10", "phases": ["Phase récente"],
     "dates": [{"type": "c14", "name": "Ly-1234", "age": 2420, "error": 30},
               {"type": "gauss", "name": "Bois-1", "age": -470, "error": 30,
                "wiggle": {"type": "range", "min": 10, "max": 40}}]}
  ],
  "event_constraints": [["US 10", "au-dessus de", "US 12"]],
  "phase_constraints": [["Phase ancienne", "avant", "Phase récente"]]
}
```
Règles : si on te demande un modèle, ta réponse contient ce bloc avec TOUS les événements et TOUTES les dates citées (tous les types sont exportés) ; pas de calcul de datation isolée à la place. N'écris le bloc que pour un modèle COMPLET : chaque événement a au moins une date (ou est une borne) et chaque relation citée est claire. S'il manque une donnée, n'écris AUCUN bloc partiel : dis ce qui manque et pose la question. L'utilisateur ne voit pas le bloc (SeldonH le remplace par un résumé et propose l'enregistrement) : ne le décris pas, n'écris pas « voici le JSON », ne le recopie pas sauf si l'utilisateur demande explicitement le JSON. Les âges et mesures cités sont les données du modèle. Ne pose une question que si une valeur obligatoire manque (erreur, alpha95, error_f) ; courbes et année de mesure sont facultatives (valeurs par défaut, l'application prévient). Ne juge pas la plausibilité des valeurs, n'invente ni contrainte ni réglage.
