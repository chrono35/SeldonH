---
name: Ordre de deux dates (laquelle est la plus ancienne ?)
description: Savoir si une date A est antérieure à une date B : probabilité d'antériorité, écart B − A, rôle de la stratigraphie et des traces ChronoModel
keywords: avant l'autre, apres l'autre, plus ancienne, plus ancien, plus recente, plus recent, anterieur, anterieure, posterieur, posterieure, anteriorite, posteriorite, antero-posteriorite, ordre chronologique, ordre des dates, laquelle, lequel, precede, succede, ecart entre, intervalle entre deux dates, recouvrement, chevauchement, older, younger, earlier, later, before or after, which is older, chronological order, relative order, overlap
priority: 9
---
**Question : A est-elle antérieure à B ?** Ce n'est pas « les intervalles se recouvrent ou non ». La bonne mesure est la **probabilité P(A antérieure à B)** (avec P(B antérieure à A) = 1 − P), et la densité de l'**écart B − A** (positif = B plus récent). Des HPD qui se recouvrent peuvent donner une probabilité nette (ex. 85 %) ; des HPD séparées donnent une probabilité proche de 100 %.

**Trois situations, trois méthodes :**
1. **Deux dates indépendantes** (aucune autre information ne les relie) : SeldonH calcule exactement P(A < B) = ∫ f_A(a) · P(B > a) da et l'écart sur les densités calibrées. Outil `comparer_dates`, ou bloc :
```calcul
{"method":"order","a":{"method":"c14","name":"<nom A>","age":<âge A>,"error":<erreur A>},"b":{"method":"c14","name":"<nom B>","age":<âge B>,"error":<erreur B>}}
```
   Chaque date peut être d'une autre méthode (tl, am, gauss, unif…), un événement (`{"method":"event","dates":[…]}`) ou une combinaison (`{"method":"combine","dates":[…]}`). Si l'utilisateur donne les deux mesures dans sa question, SeldonH calcule seul : commente le résultat.
2. **Dates d'un même modèle ChronoModel** (phases, autres dates, contraintes) : les dates a posteriori ne sont plus indépendantes. Exporter les traces (Chain_all_Events.csv), les joindre, puis outil `analyser_traces` avec `op` « order » (calcul tirage par tirage).
3. **La stratigraphie dit déjà l'ordre** (A sous B, A coupée par B) : c'est une information, à mettre comme **contrainte** dans le modèle (A plus ancien → B). Avec la contrainte, P = 100 % par construction : ce n'est pas une vérification. Pour tester si les datations sont compatibles avec la stratigraphie, faire le calcul **sans** la contrainte (cas 1 ou modèle sans contrainte) et regarder P.

**Interpréter :** c'est une probabilité a posteriori, pas un test de signification ni une p-value ; ne fixe pas de seuil à la place de l'utilisateur. Une calibration multimodale (plateau de la courbe, ex. Hallstatt 800–400 av. J.-C.) peut laisser l'ordre incertain même avec des mesures précises ; deux âges 14C proches sur un plateau peuvent donner une probabilité proche de 50 %. Si A et B datent le même événement, ce n'est pas une question d'ordre : combiner ou calculer un Event.

**Ne fais jamais** le calcul de tête (ni à partir des médianes, ni avec une formule de différence de gaussiennes sur des âges BP non calibrés).
