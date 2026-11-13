---
name: Courbe âge-profondeur et tableau CSV
description: Modèle ChronoModel d'une courbe âge-profondeur (carotte) et lecture d'un tableau CSV au format d'import de ChronoModel
keywords: modele d'age, courbe age-profondeur, age-profondeur, age profondeur, profondeur, carotte, carottage, sondage, niveaux, age-depth, depth, core, title, dating name, err depth, param a, csv
priority: 45
---
Une courbe âge-profondeur est un MODÈLE ChronoModel : écris le bloc « chronomodel » (format de la fiche « Créer un modèle ChronoModel »), jamais de bloc « calcul ».

- À la racine : `"curve":"age_depth"`. Chaque niveau est un événement avec `"depth"` (profondeur telle que donnée, AVEC son signe : 13 pour « à 13 cm », -13 pour « -13 cm »), `"depth_error"` et ses datations dans `"dates"`. Point par lequel la courbe doit passer : `"node":true`. Convention de ChronoModel : la profondeur la plus petite (valeur négative permise) est l'Event le plus ancien ; dans un texte, l'application écrit en négatif des profondeurs toutes positives (sous la surface) et garde telles quelles des profondeurs signées ; profondeur mesurée depuis la base (0 au fond) : `"depth_sign":"as_given"`.
- Ex. : `{"name":"13 cm","depth":13,"depth_error":2,"color":"bleu","dates":[{"type":"c14","name":"Ly-8812","age":310,"error":50}]}`. Couleur : seulement sur le niveau où elle est écrite.
- **Surface** (niveau actuel, sommet de la carotte) : c'est le présent de l'étude, une BORNE à la fin de la période d'étude (`tmax`), jamais un intervalle uniforme sur la période : `{"name":"surface","type":"bound","known_fixed":<tmax>,"depth":4,"depth_error":1,"node":true}`. Si la période d'étude n'est pas donnée, demande-la.
- **Stratigraphie** : une carotte est par nature une stratigraphie, le plus profond (profondeur la plus petite pour ChronoModel) est le plus ancien. `event_constraints` = UNE séquence du plus ancien au plus récent, ex. `[["90 cm","50 cm","10 cm","surface"]]`. Si l'ordre des niveaux n'est pas écrit (ou s'il est douteux : remaniement, inversion), pose la question au lieu de supposer.
- Réglages écrits par l'utilisateur : recopie-les (`"settings":{"tmin":…,"tmax":…,"step":…}`, « pas de recuit » → `"mcmc":{"anneal_recurrence":0}`).
- Sans nom donné, nomme le niveau par sa profondeur (« 13 cm »).

Tableau CSV de ChronoModel (ligne `Title;nom.chr`, puis `method;dating name/code;param a;param b;param c;Depth;Err depth`) :
- une ligne = un événement avec UNE date ; nom de l'événement = titre sans « .chr » suivi de « -1 », « -2 »… dans l'ordre des lignes ; `name` du projet = titre sans « .chr » ;
- `dating name/code` = `name` de la date (même s'il se répète) ;
- Unif : a = min, b = max ; 14C : a = âge BP, b = erreur, c = courbe (`intcal20.14c` → `"curve":"intcal20"`) ; Gauss : a = âge, b = erreur ;
- Depth → `depth` (tel quel, avec son signe, même toutes positives), Err depth → `depth_error` ; colonnes Depth présentes → `"curve":"age_depth"` et `"depth_sign":"as_given"` à la racine. Convention de ChronoModel : la profondeur la plus petite est l'Event le plus ancien (parfois 0 au fond, croissante avec le temps) ; les lignes peuvent être dans le désordre : l'ordre vient des profondeurs, pas des lignes.
Recopie TOUTES les lignes, sans en omettre ni en inventer.
