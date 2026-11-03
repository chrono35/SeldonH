---
name: Tableaux de datations (sélection, extraction, datation, événement, modèle)
description: Agir sur un tableau joint ou collé (.xlsx, .ods, .csv) que SeldonH a lu : choisir des lignes et les extraire, les dater, en faire un événement, une combinaison ou un modèle ChronoModel
keywords: tableau, table, feuille, classeur, excel, xlsx, ods, csv, lignes, colonne, colonnes, extrais, extraire, extraction, filtre, filtrer, selectionne, selection, garde, seulement les, uniquement les, toutes les dates, chaque ligne, regroupe, par US, spreadsheet, sheet, rows, column, extract, filter, select
priority: 8
---
**SeldonH a lu le tableau et le garde.** Son résumé est dans la conversation : bloc « [Tableau courant « … »] » avec les colonnes (rôle reconnu : nom, méthode, âge, erreur…), les valeurs des autres colonnes (US, site, matériau…) et les lignes numérotées **#1, #2…**. Les valeurs sont prises dans le tableau par SeldonH : **n'écris jamais toi-même les âges, les erreurs ou les lignes**, même pour une seule date. Ne recopie pas le tableau dans ta réponse : SeldonH affiche ce qu'il calcule.

**Demander une action :** outil `tableau` (modèle distant) ou bloc :
```calcul
{"method":"table","action":"<extract | date | event | combine | model>", …sélection…}
```
- `extract` : affiche les lignes choisies (avec un lien d'export CSV). Pour « extrais / liste / montre / garde seulement… ».
- `date` : calibre chaque ligne ; récapitulatif (HPD, médiane) et graphique. Pour « date / calibre les … ».
- `event` : un événement (MCMC) avec les lignes choisies ; `name` = nom de l'événement ; `mcmc` seulement si l'utilisateur donne des réglages.
- `combine` : combinaison (produit des vraisemblances) des lignes choisies.
- `model` : modèle ChronoModel (.chr) proposé à l'enregistrement ; un événement par ligne, ou par valeur de la colonne `group_by` (« un événement par US » → `"group_by":"US"`). Ni phase ni contrainte.

**Sélection** (tous les critères à la fois ; aucun = toutes les lignes) :
- `methods` : `["c14"]`, `["tl"]`, `["c14","f14c"]` (c14, f14c, tl, am, gauss, unif) ;
- `rows` / `exclude_rows` : numéros #, ex. `[2,3,5]` ou `"2-5"` ;
- `names` / `exclude_names` : noms de la colonne de nom (codes de laboratoire) ;
- `where` : conditions sur une colonne, avec son en-tête exact : `[{"column":"US","op":"=","value":"12"}]` ; op : `=`, `!=`, `contains`, `not_contains`, `<`, `>`, `<=`, `>=`, `in` (value = liste). Ex. « sur os » → `{"column":"Matériau","op":"=","value":"os"}` ; « plus anciennes que 2500 BP » → `{"column":"Âge","op":">","value":2500}`.

Exemples : « date les 14C de l'US 12 sauf Ly-1234 » →
`{"method":"table","action":"date","methods":["c14"],"where":[{"column":"US","op":"=","value":"12"}],"exclude_names":["Ly-1234"]}` ;
« un modèle avec un événement par US » → `{"method":"table","action":"model","group_by":"US"}`.

Lignes non datables (erreur, année de mesure TL, α95 manquants) : SeldonH les écarte et le dit ; demande la valeur manquante à l'utilisateur au lieu de l'inventer. Si une colonne citée par l'utilisateur n'existe pas, dis-le (colonnes du résumé) au lieu de deviner.
