---
name: Tracer des courbes
description: Format du bloc plot pour afficher une courbe dans le panneau graphique
keywords: tracer, trace, graphique, courbe, courbes, plot, dessiner, afficher, visualiser, diagramme, densite, draw, graph, chart, curve, curves, display, show, visualize, visualise, diagram, density
priority: 8
---
**Une datation calculée par SeldonH (14C, F14C, TL, Gauss, uniforme, archéomagnétisme) est tracée automatiquement** par l'application (courbe bleue, région HPD à 95 % remplie) : n'écris alors aucun bloc « plot », et ne décris pas le graphique au-delà de ce que dit la fiche de résultat.

Le bloc « plot » sert uniquement à tracer des valeurs numériques explicites. Il n'accepte ni « type », ni nom de courbe de référence, ni « inclination » : seulement `x` et `series` avec les valeurs `y`. Un bloc invalide n'est pas dessiné.

Pour afficher une courbe dans le panneau de droite, ajoute un bloc de code de langage « plot » contenant un JSON valide, comme dans l'exemple ci-dessous. Ne produis des courbes que pour des valeurs fournies par l'utilisateur ou calculées par une formule explicite que tu donnes ; n'invente pas de données.

```plot
{
  "title": "Densité gaussienne",
  "xlabel": "Année",
  "ylabel": "Densité",
  "fill": true,
  "hpd": 95,
  "x": [-600, -580, -560, -540, -520, -500, -480, -460, -440],
  "series": [
    {"name": "Ech-1 (−520 ± 35)", "y": [0.00084, 0.00262, 0.00593, 0.00968, 0.0114, 0.00968, 0.00593, 0.00262, 0.00084]}
  ]
}
```
`x` commun à toutes les séries, ou `x` propre à chaque série. Au moins 2 points par série, même longueur pour x et y. Pour une gaussienne N(μ, σ) : densité(x) = exp(−(x−μ)²/(2σ²)) / (σ√(2π)).

Avec `"fill": true`, seule la **région HPD** de chaque densité est remplie (95 % par défaut ; `"hpd": 68` pour 68 %, `"hpd": 0` pour tout remplir). C'est voulu : une densité calendaire n'est pas gaussienne, on remplit les années les plus probables, pas un intervalle ± σ. Ne mets `fill` que pour des densités de probabilité, pas pour d'autres courbes.

Une densité (`fill`) est tracée **sans échelle verticale** (elle n'a pas d'unité utile ; `"yaxis": true` pour la réafficher) et dans le bleu de ChronoModel. Une autre couleur ne se donne que si l'utilisateur la demande : `"color": "#e1572a"` dans la série concernée.
