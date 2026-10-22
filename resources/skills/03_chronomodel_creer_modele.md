---
name: Créer un modèle ChronoModel
description: Format du bloc chronomodel (JSON simplifié) pour générer un fichier .chr
keywords: wiggle, decalage, delta, wiggle-matching, modele, modeles, creer un modele, construire un modele, faire un modele, fais moi un modele, fichier chr, .chr, exporter, generer, modeliser, modelisation, evenement, evenements, phase, phases, tableau de dates, importer dans chronomodel, projet chronomodel, contrainte, contraintes, stratigraphie, plus ancien, plus ancienne
priority: 100
---
Pour créer un modèle, ÉCRIS un bloc de code de langage « chronomodel » (JSON valide, sans commentaire), exactement dans ce format. L'application complète tous les autres champs du fichier .chr. N'invente aucun autre format.

- `name` : nom du projet. `phases` : `[{"name":"Phase 1"}]` (facultatif).
- `events` (obligatoire) : liste de `{"name":"US 12","phases":["Phase 1"],"dates":[…]}` (nom unique) ; une borne : `{"name":"B","type":"bound","known_fixed":-700}`.
- Types de dates :
  - gauss : `{"type":"gauss","name":"E1","age":-520,"error":35}` (année calendaire, error = 1σ > 0)
  - unif : `{"type":"unif","name":"TPQ-TAQ","min":-600,"max":-450}`
  - tl (luminescence) : `{"type":"tl","name":"TL-1","age":488,"error":19,"ref_year":2025}` ; âge en années AVANT ref_year (année de la mesure, si l'utilisateur la donne)
  - am (archéomagnétisme) : `{"type":"am","name":"Four","process":"IF","inc":65,"alpha95":1,"field":54.2,"error_f":0.8}` ; process = I (inclinaison), D (déclinaison), F (intensité), ID (directionnel), IF ou IDF (vectoriel) ; I demande `inc` + `alpha95`, D demande `dec` + `inc` + `alpha95` (l'erreur sur D dépend de I), F demande `field` (µT) + `error_f`. Courbes facultatives : `curve_i`, `curve_d`, `curve_f` (ex. « gal2002sph2014_i »).
- `wiggle` (facultatif, sur n'importe quelle date) : décalage δ entre la date de l'échantillon t et l'événement, l'événement étant daté par t + δ. Trois formes :
  - fixe : `"wiggle": {"type": "fixed", "value": -10}`
  - intervalle (δ uniforme) : `"wiggle": {"type": "range", "min": -10, "max": 10}`
  - gaussien : `"wiggle": {"type": "gaussian", "mean": 1000, "error": 200}`
  Ne mets un wiggle que si l'utilisateur donne un décalage ; garde le signe qu'il donne (δ > 0 : l'événement est plus récent que l'échantillon). Une même date ne porte qu'un wiggle ; des dates d'un même événement peuvent avoir des wiggles de types différents.
- `event_constraints` : `[["US 12","US 10"]]` = US 12 est plus ancien que US 10 ; `phase_constraints` : idem. `settings` (facultatif) : `tmin`, `tmax`, `step`.

Exemple :
```chronomodel
{
  "name": "Site test",
  "phases": [{"name": "Phase ancienne"}, {"name": "Phase récente"}],
  "events": [
    {"name": "US 12", "phases": ["Phase ancienne"],
     "dates": [{"type": "gauss", "name": "Ech-1", "age": -520, "error": 35}]},
    {"name": "US 10", "phases": ["Phase récente"],
     "dates": [{"type": "gauss", "name": "Ech-2", "age": -430, "error": 40},
               {"type": "unif", "name": "TPQ-TAQ", "min": -480, "max": -380},
               {"type": "gauss", "name": "Bois-1", "age": -470, "error": 30,
                "wiggle": {"type": "range", "min": 10, "max": 40}}]}
  ],
  "event_constraints": [["US 12", "US 10"]],
  "phase_constraints": [["Phase ancienne", "Phase récente"]]
}
```
Règles : si on te demande un modèle, ta réponse contient ce bloc, avec TOUS les événements et TOUTES les dates citées (un événement peut avoir plusieurs dates de types différents) ; pas de calcul de datation isolée à la place, pas de simple commentaire. Les âges et mesures cités sont les données du modèle. Ne pose une question que si une valeur obligatoire manque (erreur d'une date, alpha95, error_f) ; courbes et année de mesure sont facultatives (l'application met des valeurs par défaut et prévient). Ne juge pas la plausibilité des valeurs, n'invente ni contrainte, ni étape, ni réglage de ChronoModel. Âges en années calendaires signées. Les dates radiocarbone et les densités tabulées ne sont pas exportées : dis de les saisir dans ChronoModel (plugin 14C ou Density) après ouverture du fichier.
