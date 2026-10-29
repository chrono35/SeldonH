---
name: Analyser les phases, les successions et les Events d'un calcul ChronoModel
description: Begin, End, Duration, Phase Time Range, Transition Range, Gap, hiatus ; tableau synthétique des phases et des Events
keywords: phase, phases, debut, fin, duree, durees, begin, end, duration, time range, phase time range, succession, successions, hiatus, gap, gap range, transition, transition range, lacune, interruption, tableau, tableau synthetique, synthese, synthetique, recapitulatif, event, events, evenement, evenements, posterior event date, start, table, summary table
priority: 25
---
**Les valeurs viennent de SeldonH** : quand des résultats ChronoModel ont été joints (dans cette question ou plus tôt dans la conversation), SeldonH en extrait les valeurs exactes (bloc « [Phases, successions et Events — valeurs exactes …] »). Utilise-les ; ne demande **jamais** à l'utilisateur de les recopier ou d'exporter le fichier s'il les a déjà joints. Si aucun bloc de résultats n'est présent, demande de joindre le fichier `Log_Posterior_Distrib_Stats.html` (onglet Résultats › Log de ChronoModel), pas de saisir les valeurs.

**Tableau demandé** : si le bloc dit que le tableau est « DÉJÀ affiché par SeldonH », ne le recopie pas (il est complet et exact). Écris seulement un commentaire court : 3 à 6 points tirés des « Remarques » et du détail, en citant les phases par leur nom exact. Ne dis pas qu'une valeur manque si elle est dans le tableau ; ne dis pas « non transmis » à l'utilisateur, dis « voir le tableau ».

**Variables d'une phase** (densités a posteriori ; MAP, moyenne ± écart-type, quartiles, HPD au niveau donné par ChronoModel) :
- **Begin** (début, α) = date du plus ancien Event de la phase ; **End** (fin, β) = date du plus récent ; **Duration** = End − Begin en années, toujours ≥ 0.
- **Phase Time Range** à 95 % : le plus court intervalle [a ; b] tel que la phase entière (début et fin) y soit comprise avec cette probabilité. C'est la « position » de la phase sur le calendrier ; il est plus large que les HPD du début et de la fin prises séparément.
- En **Age Cal. BP** (ou b2k), la plus grande valeur est la plus ancienne : le début a une valeur plus grande que la fin. Ne compare pas les valeurs sans en tenir compte.

**Successions** (contrainte d'ordre entre deux phases, de la plus ancienne à la plus récente) :
- **Hiatus minimal** (« Min Hiatus ») : écart minimal imposé *a priori* par l'utilisateur entre la fin de la première et le début de la seconde ; « unknown » = non fixé (seul l'ordre est imposé).
- **Transition Range** : le plus court intervalle contenant, avec la probabilité indiquée, la fin de la phase ancienne **et** le début de la phase récente : la période où se fait le passage.
- **Gap Range** : intervalle qui, avec la probabilité indiquée, sépare la fin de la phase ancienne et le début de la phase récente : un **hiatus démontré** par les données. « No Gap » : aucun hiatus n'est établi à ce niveau ; cela ne prouve pas une continuité, les deux phases peuvent être contiguës ou leurs limites trop imprécises.

**Pièges d'interprétation** (repris des remarques calculées) :
- Phase à **un seul Event** : début = fin = date de l'Event, durée nulle **par construction**. Ce n'est pas une phase brève mesurée ; ne commente pas sa durée, seulement sa date.
- Durée dont la HPD **part de 0** : la phase peut être très courte ; la borne basse n'est pas contrainte.
- Début ou fin **collé à une limite** (valeur égale à la borne de l'échantillon, souvent la période d'étude) : non contraint par les données de ce côté ; la valeur, et donc la durée et le Time Range, dépendent de la période d'étude choisie. Le dire explicitement.
- HPD **en plusieurs morceaux** : densité multimodale ; donne chaque morceau avec sa probabilité.
- Le fichier ne contient que des HPD à 95 % (ou au niveau réglé dans ChronoModel) : n'invente pas de HPD à 68 %.

**Events** : la date a posteriori d'un Event (« Posterior Event Date ») se lit comme une phase : MAP, moyenne ± écart-type, quartiles, HPD. Un Event marqué ⚠ (convergence hors critères) : signale que sa forme est à lire avec prudence.

Ces probabilités sont conditionnelles au modèle (dates, contraintes, a priori). Ne propose ni de retirer, ni de déplacer des dates ou des phases, ni d'ajouter des contraintes.
