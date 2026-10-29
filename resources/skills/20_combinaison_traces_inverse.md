---
name: Combinaison, traces MCMC et calibration inverse
description: Combiner des datations d'une même date (Combine), calculer durées et dates sur les traces MCMC de ChronoModel, mesure attendue pour une date (calibration inverse)
keywords: combine, combiner, combinaison, combinaisons, combined, combination, ward, wilson, produit des vraisemblances, traces, trace, chaines, chain_all, chain_all_events, chain_all_phases, duree, durees, duree de phase, duree entre, ecart entre, entre deux events, date entre, debut de phase, fin de phase, posterieur, anterieur, calibration inverse, inverse, age 14c attendu, age radiocarbone attendu, mesure attendue, inclinaison attendue, quel age 14c, plateau
priority: 9
---
Ces calculs sont faits par SeldonH : écris un bloc de code « calcul » (JSON) ou appelle l'outil indiqué (modèle distant). N'invente aucune valeur.

**Combinaison** (outil `combiner_dates`) : plusieurs datations d'une MÊME date (même charbon, même cuisson), méthodes différentes permises, chacune pouvant avoir son `wiggle` (elle date alors t + δ). Résultat : produit des vraisemblances (« Combine » de ChronoModel).
```calcul
{"method":"combine","name":"Four 3","dates":[{"method":"c14","age":2500,"error":30},{"method":"am","process":"I","inc":64,"alpha95":1.8}]}
```
Facultatifs : `level`, `tmin`, `tmax`. « Combine », « combinaison » → `combine` ; « événement » → `event` (MCMC, dispersion propre à chaque date). Le test de Ward et Wilson renvoyé n'est qu'une **information** sur l'accord des mesures : jamais une condition pour combiner, ni une preuve.

**Traces MCMC** (outil `analyser_traces`) : sur les chaînes exportées par ChronoModel et jointes par l'utilisateur (Chain_all_Events.csv, Chain_all_Phases.csv), tirage par tirage. Noms de colonnes EXACTEMENT ceux de la liste du fichier joint ; bornes de phase « X Begin » et « X End ».
- durée de A à B (B − A, en années, avec P(B postérieur à A)) : `{"method":"traces","op":"duration","from":"US 12","to":"US 14"}` ; durée d'une phase : from « X Begin », to « X End » ;
- date entre deux colonnes (uniforme entre les deux à chaque tirage) : `{"method":"traces","op":"between","from":"Phase A End","to":"Phase B Begin"}` ;
- densité d'une colonne (début, fin de phase, Event) : `{"method":"traces","op":"value","of":"Phase A Begin"}` ;
- antéro-postérité (A antérieur à B ?) : `{"method":"traces","op":"order","from":"US 12","to":"US 14"}` ;
- appartenance de C à un intervalle (deux Events, début et fin d'une phase, ou années) : `{"method":"traces","op":"inside","of":"US 5","from":"Phase A Begin","to":"Phase A End"}`.
Ces probabilités sont des proportions de tirages a posteriori, dans le modèle de l'utilisateur : pas un test de signification, pas une p-value.
Facultatif : `level`. Sans fichier de traces joint, demande-le (Results > Export dans ChronoModel). Une durée ou une date entre des Events de traces jointes n'est JAMAIS un `event` à calculer : c'est `traces`.

**Calibration inverse** (outil `calibration_inverse`) : mesure attendue pour un échantillon d'une date calendaire.
```calcul
{"method":"inverse","type":"c14","t":-450,"curve":"intcal20"}
```
`type` : c14 (âge BP, avec l'équivalent F14C), f14c, am (avec `"component"` : I, D ou F). Date : `t` en année astronomique (−499 = 500 av. J.-C.) ou en texte (`"t":"500 av. J.-C."`), ou `t_min` et `t_max`. `t_error` et `lab_error` (erreur de mesure prévue) seulement si l'utilisateur les donne : ne les demande pas, ne les invente pas ; une date sans erreur est prise exacte. Facultatif : `level`. Ne recopie jamais le JSON dans ta réponse : SeldonH fait le calcul et l'affiche.
