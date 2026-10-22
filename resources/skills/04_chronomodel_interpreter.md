---
name: Interpréter les résultats de ChronoModel
description: HPD, intervalles de crédibilité, convergence MCMC, phases, valeurs aberrantes
keywords: interpreter, interpretation, resultat, resultats, hpd, credibilite, intervalle, posterieur, posteriori, convergence, gelman, rubin, acceptation, trace, autocorrelation, duree, debut, fin, aberrant, outlier, resume, analyser
priority: 10
---
**Lire un résultat**
- Un événement est résumé par sa distribution a posteriori : la **HPD** (région de plus haute densité) à 95 % regroupe les années les plus probables ; elle peut être formée de plusieurs intervalles disjoints (multimodalité, typique après calibration 14C).
- On donne les HPD à **95 % et à 68 %**. Ne parle jamais de « HPD à 1σ / 2σ » : les densités a posteriori ne sont pas gaussiennes (asymétriques, plusieurs modes) ; σ ne décrit que l'erreur d'une mesure. Rapporte chaque intervalle avec sa probabilité (ex. « 1963 → 1876 av. J.-C. : 90 % »), le MAP (mode), la moyenne, l'écart-type et les quartiles tels que ChronoModel les fournit.
- L'**intervalle de crédibilité** à 95 % laisse 2,5 % de probabilité de chaque côté ; il peut être plus large que la HPD.
- Ces probabilités sont conditionnelles au modèle : date + contraintes + a priori. Dire « 95 % de probabilité que l'événement soit entre … et … **dans le modèle** ».
- Comparer l'intervalle a posteriori à l'intervalle de la date seule : un fort rétrécissement vient des contraintes (à justifier), un décalage important signale une tension entre la date et le modèle.

**Phases** : lire le début (α), la fin (β) et la durée ; la durée est une distribution, pas une valeur unique. Une durée dont l'intervalle inclut 0 indique une phase possiblement très brève ou mal contrainte.

**Qualité de l'échantillonnage MCMC** (à vérifier avant de commenter les résultats ; détail dans la fiche « Lire les logs »)
- Convergence : R̂ (split-R̂ rang-normalisé) < 1,01 pour toutes les variables = bon ; de 1,01 à 1,05 = douteux ; ≥ 1,05 = non convergé.
- ESS (bulk et tail) : au moins 500 par chaîne = bon ; moins de 100 par chaîne = trop faible.
- L'adaptation vise un taux d'acceptation du Metropolis-Hastings de 42 à 46 % (cible 44 %).
- En cas de doute : corriger d'abord le modèle (dates, contraintes), puis augmenter les itérations ; vérifier la plage tmin–tmax.

**Dates aberrantes** : une date dont la distribution a posteriori est très éloignée de sa distribution initiale est suspecte (contamination, mauvais contexte, contrainte erronée). Proposer de la retester sans contrainte ou de la retirer, en le justifiant archéologiquement.

Sois prudent : ne commente que ce qui est dans les données fournies et indique clairement ce qui manque (par exemple, si on ne te donne pas les HPD, ne les invente pas).
