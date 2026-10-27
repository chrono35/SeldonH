---
name: Courbe âge-profondeur et tableau CSV
description: Modèle ChronoModel d'une courbe âge-profondeur (carotte) et lecture d'un tableau CSV au format d'import de ChronoModel
keywords: modele d'age, courbe age-profondeur, age-profondeur, age profondeur, profondeur, carotte, carottage, sondage, niveaux, age-depth, depth, core, title, dating name, err depth, param a, csv
priority: 45
---
Une courbe âge-profondeur est un MODÈLE ChronoModel : écris le bloc « chronomodel » (format de la fiche « Créer un modèle ChronoModel »), jamais de bloc « calcul ».

- À la racine : `"curve":"age_depth"`. Chaque niveau est un événement avec `"depth"` (profondeur telle que donnée, ex. 13 pour 13 cm ; l'application l'écrit en négatif, 0 = surface, convention de ChronoModel), `"depth_error"` et ses datations dans `"dates"`. Point par lequel la courbe doit passer : `"node":true`.
- Ex. : `{"name":"13 cm","depth":13,"depth_error":2,"color":"bleu","dates":[{"type":"c14","name":"Ly-8812","age":310,"error":50}]}` ; surface datée entre a et b : `{"name":"surface","depth":4,"depth_error":1,"node":true,"dates":[{"type":"unif","name":"surface","min":a,"max":b}]}`.
- Ordre : « chacun plus ancien que celui du dessus » → `event_constraints` en une séquence du plus profond au moins profond. Sans relation donnée, l'application ordonne d'après les profondeurs.
- Sans nom donné, nomme le niveau par sa profondeur (« 13 cm »).

Tableau CSV de ChronoModel (ligne `Title;nom.chr`, puis `method;dating name/code;param a;param b;param c;Depth;Err depth`) :
- une ligne = un événement avec UNE date ; nom de l'événement = titre sans « .chr » suivi de « -1 », « -2 »… dans l'ordre des lignes ; `name` du projet = titre sans « .chr » ;
- `dating name/code` = `name` de la date (même s'il se répète) ;
- Unif : a = min, b = max ; 14C : a = âge BP, b = erreur, c = courbe (`intcal20.14c` → `"curve":"intcal20"`) ; Gauss : a = âge, b = erreur ;
- Depth → `depth` (valeur absolue), Err depth → `depth_error` ; colonnes Depth présentes → `"curve":"age_depth"`.
Recopie TOUTES les lignes, sans en omettre ni en inventer.
