---
name: Que faire quand un critère de convergence ne passe pas
description: Causes et remèdes selon le symptôme (R̂ élevé, ESS faible, date aberrante, contrainte inversée)
keywords: converge, converger, remede, remedes, corriger, correction, relancer, ne passe pas, pas converge, ne converge pas, pas convergé, non converge, que faire, comment faire, ameliorer, ess faible, rhat eleve, contrainte inversee, contradiction, aberrante, outlier, iterations, burn-in, multimodal, multimodale, remedy, remedies, solution, correct, fix, rerun, run again, does not pass, does not converge, did not converge, not converged, what to do, how to fix, improve, low ess, high rhat, inverted constraint
priority: 11
---
**Règle** : corrige d'abord le **modèle** (gratuit, décisif), règle le MCMC ensuite. Allonger les chaînes ne répare pas un modèle contradictoire.

**1. R̂ ≥ 1,05 (ici 1,3 à 1,8) et ESS très faible (< 20) sur un événement avec contraintes** (traces aux bornes, σ_i et S02 énormes, chaînes dans des états opposés) : les âges contredisent l'ordre imposé. Un événement « back » (prédécesseur) daté plus ancien qu'un événement « fwd » (successeur) est suspect. Le log ne nomme pas les liens : propose les paires candidates et demande de vérifier le sens de chaque flèche dans le projet. Actions : inverser ou supprimer la contrainte fautive ; vérifier le signe BC/AD et l'unité (BP/calendaire) de la date ; vérifier que le nom de la donnée correspond à ses paramètres. Relancer à l'identique : si R̂ passe sous 1,01, le diagnostic était bon. Si la contradiction est réelle (stratigraphie contre datations), la rapporter comme résultat.

**2. Même symptôme sans contrainte** : dates incompatibles dans un événement (écart > 3 erreurs combinées) ou posterior multimodal. Un événement est un instant unique : pour des dates étalées, utiliser une **phase** ou des événements distincts ; vérifier la courbe de calibration et la plage tmin–tmax.

**3. R̂ de 1,01 à 1,05** : chaînes trop courtes ou mal adaptées. Burn-in 1000 (défaut) → 10 000+ ; itérations 100 000 (défaut) → 500 000+ ; batches d'adaptation 100 → 300 ; 4 chaînes (défaut).

**4. R̂ ✅ mais ESS ⚠️/❌** : autocorrélation, fréquente sur σ_i et S02. Multiplier les itérations d'acquisition par 5 à 10 ; simplifier le modèle. Entre 100 et 500 par chaîne (200 à 1000 avec 2 chaînes), la médiane est correcte, les queues et bornes HPD sont imprécises.

**5. σ_i médian ≫ typique, R̂ correct** : date aberrante, écartée statistiquement. Résultat valide ; vérifier la mesure, l'erreur saisie, le contexte, puis garder, corriger ou écarter en le justifiant.

**6. S02 médian ≫ 1** (≥ 10 ; modèle d'événement EDM2) : dates non contemporaines ; remplacer l'événement par une phase.

**7. Posterior multimodal** : décrire chaque mode et sa masse, jamais une moyenne qui tombe entre les modes ; si ESS tail ❌, allonger les chaînes.

Après relance : revérifier les mêmes variables. Ne conseille pas d'élargir tmin–tmax pour « faire passer » une trace qui touche les bornes.
