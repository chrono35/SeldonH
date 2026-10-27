---
name: Date — gaussienne, uniforme et autres méthodes
description: Dates gaussienne et uniforme ; dendrochronologie, TPQ/TAQ, typologie, monnaie : comment les saisir
keywords: gauss, gaussienne, loi normale, uniforme, unif, intervalle, dendrochronologie, dendro, tpq, taq, terminus, typologie, ceramique, monnaie, numismatique, texte, source ecrite, uranium, thorium, datation simple, type de date, dendrochronology, terminus post quem, terminus ante quem, typology, pottery, ceramic, coin, numismatics, written source, uniform, gaussian
priority: 4
---
- Gaussienne (âge calendaire signé ± σ, négatif = avant J.-C.) : modèle `{"type":"gauss","name":"<référence>","age":<année>,"error":<σ>}`, calcul `{"method":"gauss","age":<année>,"error":<σ>}`.
- Uniforme (intervalle) : modèle `{"type":"unif","name":"<référence>","min":<début>,"max":<fin>}` avec min < max, calcul `{"method":"unif","min":<début>,"max":<fin>}`.
- Résultat avec âge ± σ en années calendaires (dendrochronologie, U/Th…) → `gauss`.
- Terminus post quem / ante quem, monnaie, typologie, texte daté → `unif` [min ; max]. Un TPQ seul (« après 150 ») demande une borne supérieure plausible ou une contrainte d'ordre avec un autre événement.
- Une date FIXE et certaine n'est pas une date : c'est une borne (`"type":"bound"` sur l'événement, voir la fiche de création de modèle).
- Dendrochronologie : l'abattage peut être postérieur au dernier cerne si l'aubier manque ; demander s'il y a de l'aubier ou le cambium.
- Distinguer la date (mesure sur un échantillon) de l'événement (ce qu'on veut dater) : leur lien (vieux bois, résidualité) est une hypothèse archéologique à expliciter.
Remplace chaque <…> par la valeur donnée par l'utilisateur et omets un champ facultatif qu'il n'a pas donné. Dans un modèle ChronoModel, la date s'écrit avec `"type"` ; dans un bloc « calcul » ou un outil, avec `"method"` : mêmes champs.
