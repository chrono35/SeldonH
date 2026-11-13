---
name: Datation sous contrainte stratigraphique
description: Dater une US ou une couche entre deux autres, dates reliées par l'ordre des couches : modèle ChronoModel (.chr), puis traces
keywords: us, unite stratigraphique, couche, couches, niveau, niveaux, stratigraphie, stratigraphique, sequence, au dessus, au-dessus, au dessous, en dessous, au fond, sous jacent, sus jacent, scelle, recoupe, recouvre, intercale, du milieu, intermediaire, entre deux couches, layer, layers, stratum, strata, stratigraphy, stratigraphic, above, below, overlies, underlies, sealed
priority: 9
---
**Règle** : dès qu'une **contrainte temporelle** relie des dates (US au-dessus / au-dessous, recoupement, scellement, ordre connu), la réponse passe par **ChronoModel** : propose le modèle (fichier .chr), puis réponds avec les **traces** du calcul. Une couche plus haute est plus récente (sauf remaniement, à vérifier avec l'archéologue).

**Ce que vaut le calcul direct sur les dates** (à dire en une ou deux phrases, sans dire qu'il est faux) : c'est une **estimation qui ne tient pas compte de la stratigraphie**.
- Les dates calibrées une à une sont **indépendantes** : elles ignorent l'ordre des couches. Sous la contrainte, la densité de chaque date change (densité jointe conditionnée par l'ordre), d'autant plus que les dates se chevauchent.
- « Uniforme entre les deux dates » calculé sur des dates indépendantes mélange des tirages où l'ordre est inversé (min et max échangés) : le résultat est biaisé quand les densités se recouvrent.
- L'estimation est d'autant plus proche du résultat sous contrainte que les dates sont **bien espacées** : HPD à 95 % nettement disjointes, P(plus ancienne avant plus récente) ≈ 100 % calculée **sans** contrainte (calcul `relation`, op `order`). Si les dates se chevauchent, elle est à prendre avec prudence : le résultat sous contrainte vient de ChronoModel. Ne juge jamais l'espacement de tête : SeldonH le calcule.

**Durée, ordre ou écart entre deux niveaux déjà datés d'un modèle de la conversation** (« combien de temps sépare le niveau -21 du niveau -29 ? ») : SeldonH fait lui-même le calcul sur les dates indépendantes et l'affiche comme une estimation sans la stratigraphie. Présente-la comme telle et dis que le résultat avec la stratigraphie viendra des traces du modèle calculé par ChronoModel ; ne propose pas de nouveau modèle.

**Le modèle à proposer** (bloc ```chronomodel, fiche « Créer un modèle ChronoModel ») :
- un **événement par US datée**, nommé comme l'utilisateur (« US 1 », « US 3 »), avec ses dates (fiches « Date — … ») ; une date n'a de `name` que si l'utilisateur donne une référence de laboratoire (jamais « US-1 ») ;
- `event_constraints` : la séquence du plus ancien au plus récent, ex. `["US 3", "US 1"]` (US 3 au fond, US 1 au-dessus) : une US **plus haute est plus récente**, elle vient APRÈS dans la séquence ;
- une US **sans date** (l'US 2 à dater) n'est **ni un événement ni une borne** du modèle (chaque événement doit avoir une date ; une borne exige une année donnée par l'utilisateur) : ne l'écris pas dans le bloc ; elle se date ensuite avec les traces ;
- n'ajoute ni borne, ni phase, ni réglage (MCMC compris) que l'utilisateur n'a pas donnés.

**Ensuite, avec ChronoModel** (étapes courtes) :
1. Enregistrer le .chr proposé par SeldonH, l'ouvrir dans ChronoModel, lancer le calcul (MCMC).
2. Vérifier la convergence (joindre les logs à SeldonH si besoin).
3. Exporter les traces (`Chain_all_Events.csv`) : onglet Results > onglet Distrib. Options > CSV, une boîte de dialogue demande le dossier où enregistrer les fichiers ; puis les joindre à SeldonH (📎).
4. Dans **SeldonH** (pas dans ChronoModel : l'US à dater n'est pas dans le modèle), taper « date entre US 3 et US 1 » avec les traces jointes : SeldonH tire une date uniforme entre les deux Events **tirage par tirage**, contrainte comprise. C'est la densité a posteriori d'une couche intercalée sans date (loi uniforme entre ses deux voisines, conditionnellement à elles).

**Précautions** que ChronoModel permet et que le calcul direct n'a pas : la stratigraphie porte sur les **Events**, les datations ne font que les placer. Une datation en désaccord avec l'ordre des couches (inversion stratigraphique) n'est pas une erreur du modèle : ChronoModel la traite, sa variance individuelle σ_i devient grande et elle pèse peu sur l'Event. Reste à vérifier que l'échantillon date bien la couche (charbon résiduel ou intrusif : à demander à l'archéologue). Le désaccord d'une date avec la stratigraphie se mesure en calculant l'ordre **sans** la contrainte.

**Format de la réponse** : (1) la règle en une phrase ; (2) ce que vaut le calcul direct (une estimation sans la stratigraphie), avec la condition d'espacement ; (3) le bloc ```chronomodel ; (4) une phrase : la date de l'US viendra des traces. SeldonH affiche lui-même, sous le modèle, la marche à suivre détaillée (enregistrer, calculer dans ChronoModel, joindre le journal et les traces, question à poser) : ne la récris pas. Si l'utilisateur demande les étapes, donne celles ci-dessus, sans inventer de menu ni de bouton de ChronoModel. Ne donne aucune date pour l'US à dater : elle viendra des traces. Ne réécris pas les calibrations déjà affichées.
