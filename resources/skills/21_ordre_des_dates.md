---
name: Relations entre dates (ordre, durée, date entre, appartenance)
description: Laquelle de deux dates est la plus ancienne, durée ou écart entre deux dates, date d'un niveau entre deux dates, une date dans un intervalle, corrélation entre dates ; dates simples, traces ChronoModel ou les deux
keywords: correlation, correlations, matrice de correlation, nuage de points, covariance, scatter, avant l'autre, apres l'autre, plus ancienne, plus ancien, plus recente, plus recent, anterieur, anterieure, posterieur, posterieure, anteriorite, posteriorite, antero-posteriorite, ordre chronologique, ordre des dates, laquelle, lequel, precede, succede, ecart entre, duree entre, combien de temps entre, temps ecoule, intervalle entre deux dates, date entre, situe entre, compris entre, appartient, recouvrement, chevauchement, older, younger, earlier, later, before or after, which is older, how long between, time between, chronological order, relative order, overlap
priority: 9
---
Un seul calcul, `relation` (outils `comparer_dates` ou `analyser_traces`), sur deux termes A et B (trois pour `inside`). Chaque terme est **une datation** (objet avec `method` : c14, f14c, tl, gauss, unif, am, density, ou `event` / `combine`), **une colonne des traces jointes** (nom exact : « US 12 », « Phase A End ») ou **une année fixe** (« -500 »).
```calcul
{"method":"relation","op":"<op>","from":{"method":"c14","name":"<nom A>","age":<âge A>,"error":<erreur A>},"to":{"method":"c14","name":"<nom B>","age":<âge B>,"error":<erreur B>}}
```
`op` : `order` (P(A antérieure à B), P(B antérieure à A), écart B − A), `duration` (durée B − A en années, P(B postérieure à A)), `between` (date d'un niveau entre A et B, uniforme entre les deux), `inside` (P(C dans [A ; B]) : C dans `of`), `value` (densité de `of`), `correlation` (2 à 8 termes dans `terms` : matrice r de Pearson / ρ de Spearman et nuage de points ; une datation hors modèle a une corrélation nulle par construction). Si l'utilisateur donne les mesures dans sa question, SeldonH calcule seul : commente le résultat.

**Ordre : ce n'est pas « les intervalles se recouvrent ou non ».** La bonne mesure est P(A antérieure à B). Des HPD qui se recouvrent peuvent donner une probabilité nette (ex. 85 %) ; des HPD séparées une probabilité proche de 100 %.

**Trois situations :**
1. **Dates indépendantes** (aucune autre information ne les relie) : calcul exact sur les densités calibrées (`between` : par tirages).
2. **Dates d'un même modèle ChronoModel** (phases, contraintes) : elles ne sont plus indépendantes. Exporter les traces (Chain_all_Events.csv, Chain_all_Phases.csv), les joindre, et donner les **colonnes** comme termes : calcul tirage par tirage.
3. **Une date hors du modèle et des traces** (ex. durée entre la fin d'une phase et une date 14C nouvelle) : mélange permis, la date est tirée à chaque tirage et supposée indépendante du modèle. Si elle fait déjà partie du modèle, prendre sa colonne de traces (sinon elle compte deux fois). Un `event` calculé par SeldonH n'a ni les contraintes ni les phases du modèle.

**Stratigraphie :** si elle dit déjà l'ordre (A sous B), c'est une **contrainte** du modèle ; avec la contrainte, P = 100 % par construction. Pour tester la compatibilité, calculer **sans** la contrainte et regarder P.

**Interpréter :** probabilité a posteriori, pas un test de signification ni une p-value ; ne fixe pas de seuil à la place de l'utilisateur. Un plateau de calibration (ex. Hallstatt 800–400 av. J.-C.) peut laisser l'ordre incertain même avec des mesures précises. Si A et B datent le même événement, ce n'est pas une question d'ordre : combiner ou calculer un Event. **Ne fais jamais** le calcul de tête (ni sur les médianes, ni par différence d'âges BP non calibrés).
