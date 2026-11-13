---
name: Créer un modèle ChronoModel
description: Format commun du bloc chronomodel (événements, bornes, phases, contraintes) pour générer un fichier .chr
keywords: borne, bornes, bound, date historique, eruption historique, historical date, duree de phase, duree maximale, span uniforme, uniform span, couleur, en bleu, en rouge, color, recuit, annealing, wiggle, decalage, delta, wiggle-matching, modele, modeles, creer un modele, construire un modele, faire un modele, fais moi un modele, fichier chr, .chr, exporter, generer, modeliser, modelisation, evenement, evenements, phase, phases, tableau de dates, importer dans chronomodel, projet chronomodel, contrainte, contraintes, stratigraphie, plus ancien, plus ancienne, offset, model, models, create a model, build a model, make a model, chr file, export, generate, modelling, modeling, event, events, table of dates, import into chronomodel, chronomodel project, constraint, constraints, stratigraphy, older, younger, anterieur, posterieur, au dessus, en dessous, sous jacent, precede, succede, recoupe, recouvre, overlies, underlies, older than, younger than
priority: 50
---
Pour créer un modèle, ÉCRIS un bloc de code de langage « chronomodel » (JSON valide, sans commentaire) ; l'application complète le fichier .chr. Le format de chaque type de date est dans les fiches « Date — … » jointes : n'utilise que leurs noms de champs.

- `name` : nom du projet.
- `events` (obligatoire), un objet par événement, nom unique : `{"name":"<nom>","dates":[<dates>]}` ; facultatifs `"phases":["<phase>"]` et `"color":"<couleur>"`.
- **Borne** (date fixe connue, donnée par l'utilisateur : « une borne à 40 BC ») : c'est un ÉVÉNEMENT sans dates, `{"name":"<nom>","type":"bound","known_fixed":<année>}` : année astronomique (40 BC → -39) ou en texte, `"known_fixed":"40 BC"`. Jamais dans `dates`. Jamais pour une US ou un événement à dater dont l'année n'est pas donnée : n'invente pas de `known_fixed`.
- **Date historique** (« éruption historique de 1718 », « incendie de 1563 », année d'un événement connu) : ce n'est PAS une borne, sauf si l'utilisateur le dit. C'est une datation uniforme de l'année à l'année suivante, dans l'Event : `"dates":[{"type":"unif","min":1718,"max":1719}]` (ou l'intervalle donné par l'utilisateur). Dis-le en une phrase et demande de valider ou de préciser l'intervalle.
- `color` : recopie la couleur demandée (« en bleu » → `"bleu"`, `"rouge"`, `"#ff0000"`) sur l'ÉVÉNEMENT, même si elle est citée avec une de ses dates.
- `name` d'une date : la référence ou le code donné par l'utilisateur, où qu'il soit dans la phrase (« référence Ly-12345 », « réf Bdx-478 », « am Rennes2025 »). Sans référence donnée, pas de `name` : n'en invente jamais (« Ref-1 », « Ech-1 »…).
- `phases` (facultatif) : `[{"name":"<phase>"}]` ; un événement y est rattaché par `"phases":["<phase>"]`. Durée de la phase, SEULEMENT si l'utilisateur la donne : durée maximale connue (« la phase dure au plus 20 ans ») → `"duration_max": 20` ; « span uniforme » → `"uniform_span": true`. Sinon rien (durée inconnue).
- `wiggle` (facultatif, sur une date, seulement si l'utilisateur donne un décalage δ ; l'événement est daté par t + δ, garde le signe donné) : `{"type":"fixed","value":<δ>}`, `{"type":"range","min":<a>,"max":<b>}` ou `{"type":"gaussian","mean":<m>,"error":<e>}`.
- `event_constraints` (entre événements) et `phase_constraints` (entre phases) : `["A", "<relation>", "B"]` en RECOPIANT le mot de l'utilisateur, l'application en déduit le sens (français ou anglais) : avant / après, antérieur / postérieur, plus ancien / plus récent, précède / succède, en dessous / au-dessus, sous / sur, coupe, recoupe, recouvre, scelle, est coupé par, before / after, below / above, cuts, is cut by… Une séquence `["A", "B", "C"]` va du plus ancien au plus récent. Hiatus entre deux PHASES donné par l'utilisateur, en dernier élément : `["A", "avant", "B", {"hiatus": 50}]` (hiatus d'au moins 50 ans) ou `{"hiatus_min": 10, "hiatus_max": 30}` (hiatus minimal entre 10 et 30) ; jamais entre des événements. Reprends TOUTES les relations citées. Relation sans ordre clair (« contemporain ») : pose la question.
- Courbe âge-profondeur (carotte, profondeurs) ou tableau CSV de ChronoModel : suis la fiche « Courbe âge-profondeur et tableau CSV ».
- **Courbe archéomagnétique** (construire une courbe I, D, F à partir de mesures datées) : `"curve"` à la racine (`"vector"` I, D, F ; `"spherical"` I, D ; `"unknown_dec"` I, F ; `"inclination"`, `"declination"`, `"intensity"`). Chaque Event (borne comprise) porte SA mesure sur l'événement, pas dans `dates` : `"inc"`, `"dec"`, `"alpha95"` (degrés), `"field"`, `"error_f"` (µT), recopiés du texte. Ses `dates` restent ses datations (c14, gauss, unif…), jamais une date « am ».
- `settings` (facultatif) : `tmin`, `tmax`, `step`. `mcmc` (seulement ce que l'utilisateur donne) : `num_iter`, `num_burn`, `num_proc`, `thinning_interval`, `seeds`… ; « pas de recuit » → `"mcmc": {"anneal_recurrence": 0}`.

Exemple de FORME (valeurs fictives, ne les reprends jamais) :
```chronomodel
{
  "name": "Exemple",
  "events": [
    {"name": "A", "color": "vert",
     "dates": [{"type": "c14", "name": "Lab-0000", "age": 1000, "error": 10}]},
    {"name": "B", "type": "bound", "known_fixed": 500}
  ],
  "event_constraints": [["B", "avant", "A"]]
}
```
Règles : si on te demande un modèle, ta réponse contient ce bloc avec TOUS les événements, TOUTES les dates et TOUTES les relations citées ; pas de calcul de datation isolée à la place. Chaque valeur du bloc vient de la question : n'en ajoute aucune (pas de champ facultatif inventé, pas de valeur d'exemple). N'écris le bloc que pour un modèle COMPLET : chaque événement a au moins une datation donnée par l'utilisateur (ou est une borne dont l'utilisateur donne l'année fixe) et chaque relation citée est claire. Une couche, un niveau ou une US sans datation n'est ni un événement ni une borne : ne l'écris pas. Un âge mesuré (« 4520 ± 40 BP ») est une date de l'événement, jamais une borne. S'il manque une valeur obligatoire (erreur, α95, error_f, année de mesure ref_year d'une TL), n'écris AUCUN bloc partiel : dis ce qui manque et pose la question. Les courbes sont facultatives (valeurs par défaut, l'application prévient). L'utilisateur ne voit pas le bloc (SeldonH le remplace par un résumé, le vérifie et propose l'enregistrement) : ne le décris pas et ne le recopie pas, sauf si l'utilisateur demande explicitement le JSON. Ne juge pas la plausibilité des valeurs, n'invente ni contrainte ni réglage.
