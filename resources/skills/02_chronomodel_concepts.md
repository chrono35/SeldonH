---
name: Concepts de ChronoModel
description: Événements, dates, phases, bornes, contraintes, MCMC dans ChronoModel
keywords: wiggle, decalage, wiggle-matching, chronomodel, evenement, evenements, phase, phases, borne, bounds, contrainte, contraintes, bayesien, bayesienne, mcmc, modele, prior, a priori, stratigraphie, stratigraphique, anteriorite
priority: 3
---
ChronoModel est un logiciel de modélisation chronologique bayésienne (échantillonnage MCMC).

- **Date** : une mesure (âge ± erreur) liée à un échantillon. Types de ChronoModel : Gauss (âge calendaire ± σ), Uniforme (intervalle TPQ–TAQ), 14C (calibré), TL/OSL (luminescence : âge en années avant une année de référence, ± σ), archéomagnétisme (inclinaison I, déclinaison D, intensité F, comparées à des courbes de référence), densité tabulée (densité lue dans un fichier CSV).
- **Événement** : l'instant inconnu θ à dater (dépôt d'une couche, construction, utilisation…). Un événement regroupe une ou plusieurs dates qui le datent (modèle hiérarchique : toutes les dates d'un événement partagent le même θ).
- **Wiggle (décalage δ)** : écart connu entre ce que date l'échantillon (t) et l'événement (vieux bois, cernes comptés, durée d'utilisation…). L'événement est daté par y = t + δ. Trois types : fixe (δ = valeur), intervalle (δ uniforme entre min et max), gaussien (δ ~ N(moyenne ; erreur)).
  - Date seule : ChronoModel affiche la densité de t + δ, convolution de la densité de la date par la loi de δ (simple translation pour un δ fixe).
  - Dans un événement : δ est une inconnue du MCMC ; la vraisemblance reste celle de la date sans décalage, et c'est t + δ qui est rattaché à θ. ChronoModel donne alors deux résultats par date : t (la date de l'échantillon) et t + δ (le « Wiggle », ramené à l'événement).
  - Dans le fichier .chr, le décalage est au niveau de la date (hors de `data`) : `delta_type` = -1 aucun, 0 fixe (`delta_fixed`), 1 gaussien (`delta_average`, `delta_error`), 2 intervalle (`delta_min`, `delta_max`). Seul `delta_type` compte : ChronoModel conserve d'anciennes valeurs dans les autres champs quand on revient à « aucun ».
- **Borne (Bound)** : événement dont l'âge est fixé et connu (ancrage).
- **Phase** : groupe d'événements (période d'activité). ChronoModel estime son début (α), sa fin (β) et sa durée. Une phase suppose par défaut une distribution uniforme des événements entre α et β.
- **Contraintes d'ordre** : entre événements (stratigraphie : A plus ancien que B) ou entre phases (succession, éventuellement avec un hiatus minimal). Elles font gagner beaucoup en précision quand elles sont justes, et faussent les résultats quand elles sont fausses : à vérifier avec l'archéologue. Dans le fichier .chr, une contrainte est stockée avec `bwd_id` = l'événement le plus ancien et `fwd_id` = l'événement le plus récent (vérifié sur des projets réels).
- **Plage d'étude (tmin–tmax)** : doit contenir largement toutes les dates (marge d'au moins 3σ). Le **pas** (step) est la résolution de la grille de calcul.
- **MCMC** : plusieurs chaînes (4 par défaut), période de chauffe (burn-in), adaptation, puis itérations d'acquisition (100 000 par défaut), avec amincissement (thinning).
