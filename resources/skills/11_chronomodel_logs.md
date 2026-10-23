---
name: Lire les logs de ChronoModel et repérer la variable à problème
description: Verdict de convergence, critères R̂ et ESS de ChronoModel, localisation de la variable en échec (événement, donnée, paramètre)
keywords: annealing, recuit, disabled, log, logs, journal, resultats, ne converge pas, pas converge, converge, converger, convergence, converged, not converged, rhat, gelman, ess bulk, ess tail, ess, posterior event date, posterior std ti, posterior shrinkage, posterior calib, std ti, shrinkage, adaptation, not adapted, critere, criteres, variable, probleme, echec, acceptance rate, results, does not converge, did not converge, criterion, criteria, problem, failure, failed
priority: 12
---
**Quatre logs** : *Description* (événements, données, **nombre** de contraintes « back » = a un prédécesseur plus ancien, « fwd » = a un successeur plus récent, paramètres réels des dates), *Initialisation*, *Adaptation*, *Statistiques a posteriori* (résultats et critères). Le nom d'une donnée est libre : fie-toi à ses paramètres réels.

**Procédure** : 1) lis le verdict de tête (« Not converged (max R̂ = … ; min ESS = …) »). 2) Repère toute section portant ⚠️ ou ❌. 3) Nomme chaque variable en échec par **événement › donnée n° › grandeur**. 4) Cherche la cause, puis propose des actions (fiche « critère qui ne passe pas »). Ne dis **jamais** « le modèle a convergé » s'il reste un ❌, et ne commente pas médiane ni HPD d'une variable ❌.

**Grandeurs** : *Posterior Event Date* = date θ de l'événement ; *Posterior calib. date* = date d'une donnée ; *Posterior Std ti* = σ_i de la donnée (≈ 1 si compatible, énorme si aberrante) ; *Posterior Shrinkage param.* = S02 de l'événement (≈ 1 si dates compatibles, ≫ 1 si leur dispersion dépasse leurs erreurs).

**Critères** (code de ChronoModel) :
- R̂ = split-R̂ rang-normalisé, le pire de *bulk* et *tail* : ✅ < 1,01 ; ⚠️ de 1,01 à 1,05 ; ❌ ≥ 1,05.
- ESS bulk / tail : seuils **par chaîne × nombre de chaînes** : ✅ ≥ 500 par chaîne (1000 avec 2 chaînes, 2000 avec 4) ; ⚠️ de 100 à 500 par chaîne ; ❌ < 100 par chaîne (200 avec 2 chaînes).
- **Verdict global** : « Not converged » dès qu'une variable a R̂ ≥ 1,05 ou ESS < 100 par chaîne. Entre les seuils ⚠️, c'est « Questionable convergence » si moins de 4 variables (ou ≤ 1 %) sont touchées, « Not converged » au-delà. Sinon « Satisfactory convergence ».
- 4 chaînes par défaut ; avec 2, R̂ est peu fiable. Un posterior en deux gros morceaux HPD peut être légitime (calibration multimodale).

**Repérer la variable racine** : plusieurs variables d'un même événement avec le **même R̂** → une seule cause. La racine est en général l'événement (date ou contrainte) ; un σ_i ou un S02 énorme en est la **conséquence**. Indices : trace qui touche les bornes de la période d'étude ; états de fin d'adaptation très différents entre les deux chaînes ; médiane de l'événement éloignée de ses dates de plusieurs erreurs.

**Recuit (« 4 - ANNEALING »)** : « Disabled » = recuit simulé désactivé (Recurrence = 0, demandé par « pas de recuit ») ; sinon Init. Temp. (exposant de 2), Recurrence et Dwell. Sans recuit, une chaîne peut rester bloquée dans un mode d'une densité multimodale : R̂ élevé entre chaînes, traces qui ne se croisent pas ; remède : réactiver le recuit ou allonger les chaînes.

**Adaptation** : elle s'arrête quand tous les taux d'acceptation sont dans leur fenêtre : 42–46 % (cible 44 %), et 30–40 % pour t_i et σ_i des événements à plusieurs dates. « Not adapted after 100 batches » (taux en rouge) veut dire qu'au moins un taux est resté hors fenêtre : fréquent, sans gravité en pratique si R̂ et ESS passent. Le *Shrinkage* S02 n'existe que dans le modèle d'événement EDM2.
