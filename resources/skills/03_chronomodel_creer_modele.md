---
name: Créer un modèle ChronoModel — scène des Events
description: Format commun du bloc chronomodel (événements, bornes, phases, contraintes) pour générer un fichier .chr
keywords: borne, bornes, bound, date historique, eruption historique, historical date, duree de phase, duree maximale, span uniforme, uniform span, couleur, en bleu, en rouge, color, recuit, annealing, wiggle, decalage, delta, wiggle-matching, modele, modeles, creer un modele, construire un modele, faire un modele, fais moi un modele, fichier chr, .chr, exporter, generer, modeliser, modelisation, evenement, evenements, phase, phases, tableau de dates, importer dans chronomodel, projet chronomodel, contrainte, contraintes, stratigraphie, plus ancien, plus ancienne, offset, model, models, create a model, build a model, make a model, chr file, export, generate, modelling, modeling, event, events, table of dates, import into chronomodel, chronomodel project, constraint, constraints, stratigraphy, older, younger, anterieur, posterieur, au dessus, en dessous, sous jacent, precede, succede, recoupe, recouvre, overlies, underlies, older than, younger than
priority: 50
---
Pour créer un modèle, écris un bloc de code « chronomodel » (JSON valide, sans commentaire) ; l'application en fait le fichier .chr. Champs des dates : ceux des fiches « Date — … » jointes, et eux seuls.

- `name` : nom du projet. `events` : un objet par événement, nom unique, `{"name":"<nom>","dates":[<dates>]}` ; facultatifs `"phases":["<phase>"]`, `"color"`.
- **Borne** (année fixe donnée par l'utilisateur : « une borne à 40 BC ») : un Event sans dates, `{"name":"<nom>","type":"bound","known_fixed":-39}` (ou `"known_fixed":"40 BC"`). Seulement si l'année est donnée.
- **Date historique** (« éruption de 1718 ») : une date uniforme dans l'Event, `{"type":"unif","min":1718,"max":1719}` (ou l'intervalle de l'utilisateur) ; dis-le en une phrase et fais valider l'intervalle. Une borne seulement si l'utilisateur le dit.
- `color` (« en bleu » → `"bleu"`, `"#ff0000"`) : sur l'Event, même citée avec une date.
- `name` d'une date : la référence donnée (« réf Bdx-478 ») ; sans référence, pas de `name`.
- `wiggle` (sur une date, décalage δ donné ; Event daté par t + δ, signe gardé) : `{"type":"fixed","value":<δ>}`, `{"type":"range","min":<a>,"max":<b>}` ou `{"type":"gaussian","mean":<m>,"error":<e>}`.
- `event_constraints` : `["A", "<relation>", "B"]` avec le mot de l'utilisateur recopié (avant / après, sous / sur, recoupe, scelle, before / after…), l'application en déduit le sens ; `["A", "B", "C"]` va du plus ancien au plus récent. Reprends toutes les relations citées ; « contemporain » : pose la question.
- Phases : fiche « Modèle — scène des phases ». Courbe (I, D, F, X, Y, Z) : fiche « Modèle — courbe ». Âge-profondeur, tableau CSV : fiche « Courbe âge-profondeur et tableau CSV ».
- `settings` : `tmin`, `tmax`, `step` ; `mcmc` : `num_iter`, `num_burn`, `num_proc`, `seeds`… ; « pas de recuit » → `{"anneal_recurrence": 0}`. Seulement ce que l'utilisateur donne.

Exemple de FORME (valeurs fictives, à ne pas reprendre) :
```chronomodel
{"name": "Exemple",
 "events": [{"name": "A", "dates": [{"type": "c14", "name": "Lab-0000", "age": 1000, "error": 10}]},
            {"name": "B", "type": "bound", "known_fixed": 500}],
 "event_constraints": [["B", "avant", "A"]]}
```
Règles :
- Le bloc contient tous les événements, toutes les dates et toutes les relations citées, avec les seules valeurs de la question ; pas de calcul de datation isolée à la place.
- Un Event a au moins une datation donnée (ou est une borne d'année donnée) ; une US sans datation n'est pas un Event. Un âge mesuré (« 4520 ± 40 BP ») est une date, jamais une borne.
- Valeur obligatoire manquante (erreur, α95, error_f, ref_year d'une TL) : pas de bloc ; dis ce qui manque et pose la question. Les courbes de calibration sont facultatives.
- L'utilisateur voit un résumé du bloc fait par SeldonH : ne le décris pas et ne le recopie pas (sauf JSON demandé). Ne juge pas la plausibilité des valeurs.
