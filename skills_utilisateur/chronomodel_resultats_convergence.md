---
name: Diagnostiquer les résultats d'un calcul ChronoModel (convergence, σ des dates)
description: Lire le résumé des résultats ChronoModel (export HTML du log) : critère global, variables hors critères, remèdes, dates au σ grand, inversions
keywords: resultats chronomodel, resultat du calcul, log chronomodel, export html, log_posterior, convergence, converge, not converged, r hat, rhat, ess, ess bulk, ess tail, critere, criteres, sigma, std ti, outlier, aberrante, inversion, inversion stratigraphique, diagnostic, thinning, annealing, recuit, iterations, chronomodel results, convergence diagnostics
priority: 30
---
Quand un fichier de résultats de ChronoModel est joint, SeldonH en transmet un résumé exact (bloc « [Résultats ChronoModel — résumé exact extrait par SeldonH …] »). Appuie-toi uniquement sur ces valeurs : n'en invente aucune, ne recalcule rien.

Structure de la réponse :

1. **Avis sur le critère global** : reprends la ligne « Verdict de convergence » du résumé (calculée sur les proportions), avec R̂ max, ESS min et la part des variables hors seuil. Seuils : R̂ < 1,01 ; ESS bulk et ESS tail ≥ 1500 (ou le seuil indiqué). « Not converged » ne veut pas dire « inutilisable » : ne sois pas plus sévère que le verdict.

2. **Remèdes** :
   - **R̂ mauvais** (≥ 1,01) : les chaînes n'explorent pas la même région ; augmenter le **thinning** (intervalle d'échantillonnage) et/ou ajouter ou renforcer le **recuit (annealing)** pour aider les chaînes à se mélanger, puis relancer.
   - **ESS mauvais avec un R̂ bon** : l'échantillon est trop petit ; augmenter le **nombre d'itérations d'acquisition** proportionnellement à ce qui manque : facteur ≈ seuil / ESS le plus faible (le résumé le donne, ex. « ×3,0 » pour un ESS de 504 et un seuil de 1500).
   - Un calcul plus long ne corrige pas un modèle incohérent : voir le point 5.

3. **Variables qui ne remplissent pas les critères**, dans l'ordre du résumé (de la pire à la meilleure) : nom de l'Event ou de la datation, type de variable, critère(s) en échec avec leur valeur. S'il y en a beaucoup, donne les premières et le nombre total. Recopie les noms et valeurs exactement ; ne numérote pas au-delà de la liste fournie. Donne aussi les comptes par type de variable (ligne « Par type de variable »).

4. **Ce que cela implique** :
   - Si les critères des **Events** (date de l'événement) sont bons, leurs **positions** (dates) sont fiables, même si d'autres variables sont limites.
   - Base-toi sur les comptes : si 10 dates d'Events sur 155 sont hors critères, dis « 10 sur 155 », jamais « la plupart » ni « de nombreux ».
   - Si les **σ** (σ de la date « Std ti », σ de l'événement « shrinkage ») ou les **dates calibrées** d'un Event ont de mauvais critères, la **forme** de la densité de cet Event (son étalement, ses queues, sa multimodalité) peut être mal définie : à interpréter avec prudence.



5. **Explication des résultats (σ des dates)** :
   - Cette liste est **distincte** des variables hors critères : une date au σ grand avec une « convergence bonne » est bien calculée, son σ grand est un résultat réel du modèle, pas un défaut de convergence. Ne mélange pas les deux listes.
   - Une datation dont le **σ (MAP de Posterior Std ti) est supérieur à l'écart-type de la date calibrée** s'écarte fortement de son événement : **soupçonner une date aberrante (outlier)** — contamination, mauvaise association à l'événement, erreur de saisie, courbe ou réservoir inadaptés.
   - Si **toutes les datations d'un même Event** ont un σ grand, c'est l'événement entier qui ne tient pas à sa place : **avertir d'une possible inversion stratigraphique** (contrainte d'ordre fausse ou Event mal placé dans le modèle). Pour un Event à une seule datation, les deux explications (outlier ou inversion) sont possibles. Quand le résumé signale une inversion possible, **mentionne-la obligatoirement**, avec le nom de l'Event.

Règles :
- Ne dis **jamais** de retirer une date ou un Event du modèle, même sous condition (« après vérification », « si nécessaire », « sans validation »…) : n'évoque pas le retrait du tout.
- Ne propose **pas** de déplacer, décaler (wiggle) ou modifier une date, ni d'**ajouter des contraintes** : invite seulement à vérifier les contraintes existantes.
- N'affirme rien que les nombres du résumé n'établissent pas (pas de « les Events sont bien contraints », pas de cause supposée présentée comme certaine).
- Ne dis pas « le nombre d'itérations est suffisant » si un ESS est sous le seuil.
- Recommande de **bien contrôler le modèle chronologique** : la saisie des mesures, l'association des échantillons aux événements, les contraintes stratigraphiques et l'ordre des phases, les courbes et effets réservoir. C'est à l'archéologue de décider.
- Reste concis : tableau ou liste numérotée pour les variables, une phrase par remède.
