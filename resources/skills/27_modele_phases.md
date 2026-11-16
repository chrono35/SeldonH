---
name: Modèle — scène des phases
description: Phases d'un modèle ChronoModel : rattachement des Events, durée, contraintes entre phases, hiatus
keywords: phase, phases, duree de phase, duree maximale, span uniforme, uniform span, hiatus, contrainte entre phases, phase constraints, sequence de phases, succession de phases
priority: 40
---
- `phases` : `[{"name":"<phase>"}]` à la racine ; un Event y est rattaché par `"phases":["<phase>"]` (plusieurs phases possibles).
- Durée de la phase, SEULEMENT si l'utilisateur la donne : durée maximale (« la phase dure au plus 20 ans ») → `"duration_max": 20` ; « span uniforme » → `"uniform_span": true`. Sinon rien (durée inconnue).
- `phase_constraints` (entre phases) : `["A", "<relation>", "B"]` en RECOPIANT le mot de l'utilisateur (avant / après, antérieure / postérieure, précède / succède, before / after…) ; une séquence `["A", "B", "C"]` va de la plus ancienne à la plus récente. Reprends TOUTES les relations citées.
- Hiatus entre deux PHASES donné par l'utilisateur, en dernier élément : `["A", "avant", "B", {"hiatus": 50}]` (hiatus d'au moins 50 ans) ou `{"hiatus_min": 10, "hiatus_max": 30}` (hiatus minimal entre 10 et 30) ; jamais entre des Events.
- Une phase sans Event n'existe pas : chaque phase citée reçoit au moins un Event de la question.
