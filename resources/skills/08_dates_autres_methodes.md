---
name: Autres méthodes de datation
description: Dendrochronologie, luminescence, TPQ/TAQ, typologie, comment les saisir dans un modèle
keywords: dendrochronologie, dendro, luminescence, otl, tl, thermoluminescence, tpq, taq, terminus, typologie, ceramique, monnaie, numismatique, texte, source ecrite, uranium, thorium, datation simple, type de date, dendrochronology, optically stimulated, terminus post quem, terminus ante quem, typology, pottery, ceramic, coin, numismatics, written source, simple dating, date type
priority: 4
---
Comment traduire une information en date de modèle :
- **Résultat avec âge ± σ en années calendaires** (dendrochronologie avec cernes datés, U/Th…) → date `gauss`, avec la durée d'erreur en 1σ.
- **Luminescence (TL/OSL)** : l'âge est donné en « ans avant l'année de mesure ». Dans un modèle ChronoModel, utiliser la date `tl` avec `age`, `error` et `ref_year` (année de la mesure, à demander). Pour un calcul simple dans SeldonH, la méthode `tl` convertit en années calendaires avec l'année de mesure.
- **Archéomagnétisme** : date `am` (inclinaison, déclinaison et/ou intensité, comparées à une courbe de référence) ; voir la fiche sur la création de modèle pour les valeurs à fournir.
- **Terminus post quem / ante quem (TPQ/TAQ)**, monnaie, typologie, texte daté → intervalle `unif` [min ; max] (tous les âges de l'intervalle sont équiprobables). Un TPQ seul (« après 150 ») nécessite une borne supérieure plausible ou une contrainte d'ordre avec un autre événement.
- **Dendrochronologie** : date exacte de l'année du dernier cerne ; l'événement d'abattage peut être légèrement postérieur si l'aubier est absent. Demander s'il y a de l'aubier ou le waney edge.
- **Distinguer** la date (mesure sur un échantillon) de l'événement (ce qu'on veut dater) : le lien entre les deux (ancienneté du bois, résidualité) est une hypothèse archéologique à expliciter.
