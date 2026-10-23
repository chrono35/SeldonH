---
name: Interpréter les résultats de ChronoModel
description: HPD, intervalles de crédibilité, convergence MCMC, phases, valeurs aberrantes
keywords: wiggle, decalage, interpreter, interpretation, resultat, resultats, hpd, credibilite, intervalle, posterieur, posteriori, convergence, gelman, rubin, acceptation, trace, autocorrelation, duree, debut, fin, aberrant, outlier, resume, analyser, offset, interpret, result, results, credible, interval, posterior, acceptance, duration, start, begin, end, summary, summarize, analyse, analyze
priority: 10
---
**Lire un résultat**
- Un événement est résumé par sa distribution a posteriori : la **HPD** (région de plus haute densité) à 95 % regroupe les années les plus probables ; elle peut être formée de plusieurs intervalles disjoints (multimodalité, typique après calibration 14C).
- **Convergence MCMC** (seuils de ChronoModel) : R̂ < 1,01 et ESS ≥ 500 par chaîne = satisfaisant ; R̂ < 1,05 et ESS ≥ 100 par chaîne = douteux ; au-delà = non convergé. Les dispersions σ_i et S0 mélangent souvent moins bien que θ : une ESS faible sur σ_i n'empêche pas de lire θ si son R̂ et son ESS sont bons, mais il faut le dire. Remède : plus d'itérations, plus de chaînes, vérifier l'accord des dates.
- Dans un événement, σ_i petit = la date est en accord avec les autres ; σ_i grand = date discordante (valeur aberrante, résiduelle ou intrusive), presque ignorée pour estimer θ.
- On donne les HPD à **95 % et à 68 %** ; si l'utilisateur demande un autre niveau de confiance (ex. 99 %), on le donne en premier, sans retirer les deux autres. Ne parle jamais de « HPD à 1σ / 2σ » : les densités a posteriori ne sont pas gaussiennes (asymétriques, plusieurs modes) ; σ ne décrit que l'erreur d'une mesure. Rapporte chaque intervalle avec sa probabilité (ex. « 1963 → 1876 av. J.-C. : 90 % »), le MAP (mode), la moyenne, l'écart-type et les quartiles tels que ChronoModel les fournit.
- L'**intervalle de crédibilité** à 95 % laisse 2,5 % de probabilité de chaque côté ; il peut être plus large que la HPD.
- Ces probabilités sont conditionnelles au modèle : date + contraintes + a priori. Dire « 95 % de probabilité que l'événement soit entre … et … **dans le modèle** ».
- Comparer l'intervalle a posteriori à l'intervalle de la date seule : un fort rétrécissement vient des contraintes (à justifier), un décalage important signale une tension entre la date et le modèle.

**Dates avec wiggle (décalage δ)** : pour chacune, distingue t (date de l'échantillon) et t + δ (date ramenée à l'événement) ; c'est t + δ qui se compare à θ, pas t. La « calibration » d'une telle date affichée seule est celle de t + δ. Un δ gaussien large (erreur de δ ≫ précision des autres dates) mélange mal : taux d'acceptation de t très bas, R̂ élevé sur t et σ de cette date ; conseille plus d'itérations et vérifie que le décalage donné est bien justifié.

**Phases** : lire le début (α), la fin (β) et la durée ; la durée est une distribution, pas une valeur unique. Une durée dont l'intervalle inclut 0 indique une phase possiblement très brève ou mal contrainte.

**Qualité de l'échantillonnage MCMC** (à vérifier avant de commenter les résultats ; détail dans la fiche « Lire les logs »)
- Convergence : R̂ (split-R̂ rang-normalisé) < 1,01 pour toutes les variables = bon ; de 1,01 à 1,05 = douteux ; ≥ 1,05 = non convergé.
- ESS (bulk et tail) : au moins 500 par chaîne = bon ; moins de 100 par chaîne = trop faible.
- L'adaptation vise un taux d'acceptation du Metropolis-Hastings de 42 à 46 % (cible 44 %).
- En cas de doute : corriger d'abord le modèle (dates, contraintes), puis augmenter les itérations ; vérifier la plage tmin–tmax.

**Dates aberrantes** : une date dont la distribution a posteriori est très éloignée de sa distribution initiale est suspecte (contamination, mauvais contexte, contrainte erronée). Proposer de la retester sans contrainte ou de la retirer, en le justifiant archéologiquement.

Sois prudent : ne commente que ce qui est dans les données fournies et indique clairement ce qui manque (par exemple, si on ne te donne pas les HPD, ne les invente pas).
