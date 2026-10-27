---
name: Tracer des courbes
description: Format du bloc plot pour afficher une courbe dans le panneau graphique
keywords: tracer, trace, graphique, courbe, courbes, plot, dessiner, afficher, visualiser, diagramme, densite, gaussienne, loi normale, draw, graph, chart, curve, curves, display, show, visualize, visualise, diagram, density
priority: 8
---
**Une datation calculée par SeldonH (14C, F14C, TL, Gauss, uniforme, archéomagnétisme) est tracée automatiquement** par l'application : n'écris alors aucun bloc « plot », et ne décris pas le graphique au-delà de ce que dit la fiche de résultat.

Pour tracer une **loi classique** (gaussienne, log-normale, uniforme, triangulaire, trapézoïdale, exponentielle, gamma, bêta), écris SEULEMENT la loi et ses paramètres : SeldonH calcule les points. N'écris JAMAIS de liste de valeurs `y` pour une loi : tu ne sais pas les calculer exactement.

```plot
{"title": "Gaussienne N(-500 ; 40)", "xlabel": "Année",
 "series": [{"name": "N(-500 ; 40)", "law": "normal", "mean": -500, "sd": 40}]}
```
Paramètres (`law`) : normal (mean, sd) ; lognormal (median et factor, ou meanlog et sdlog) ; uniform (min, max) ; triangular (min, mode, max) ; trapezoidal (min, mode_min, mode_max, max) ; exponential (mean) ; gamma (mean et sd) ; beta (alpha, beta, min, max). Facultatifs : `offset` (t = offset + x), `"reverse": true` (t = offset − x), `tmin`, `tmax`. Plusieurs lois : une série chacune. L'étendue est choisie par SeldonH (toute la loi) : ne donne `tmin`/`tmax` que si l'utilisateur les demande.

Valeurs **données par l'utilisateur** (tableau de mesures) : `x` commun ou propre à chaque série et `y` dans la série, même longueur, au moins 2 points. N'invente aucune valeur. Le bloc n'accepte ni « type », ni nom de courbe de référence : un bloc invalide n'est pas dessiné.

Avec `"fill": true` (par défaut pour une loi), seule la **région HPD** de chaque densité est remplie (95 % ; `"hpd": 68` pour 68 %, `"hpd": 0` pour tout remplir), sans échelle verticale (`"yaxis": true` pour la réafficher). Une couleur seulement si l'utilisateur la demande : `"color": "#e1572a"` dans la série.
