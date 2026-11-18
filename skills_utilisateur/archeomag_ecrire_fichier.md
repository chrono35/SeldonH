---
name: Écrire un fichier de mesure archéomagnétique (AMD / AMP)
description: Modèle exact d'un fichier AMD/AMP à préparer pour le magnétomètre (spécimens sans mesure)
keywords: fichier amd, fichier amp, format amd, format amp, armag, molspin, fichier de mesure, creer un fichier, cree un fichier, fais un fichier, fait un fichier, ecris un fichier, preparer un fichier, gabarit, template, specimens, write an amd file, create an amd file
priority: 7
---
Écris le fichier **exactement** comme ci-dessous, dans un seul bloc ```text : pas de commentaire, pas de `#`, pas de numéro de ligne, pas de `…`. Écris **tous** les spécimens demandés, un bloc complet chacun, sans en sauter. Garde les deux-points, les espaces et le nombre de chiffres.

```text
Spinner_Molspin 2008
Commune : <commune>
Site : <site>
Latitude  :   0°  0'  0"
Longitude :   0°  0'  0" IGRF:+00.0
Prélèvements sur matériaux déplacés
Type de carottage : à plat
Date de création : <JJ/MM/AAAA>
 
 
Id:<nom>          in:000.0 az:000.0 Tet:000.0 Psy:000.0 v:<volume> com:TH 0.0°T <forme>
Repère:
CompDes:  T1:0000T+  T2:0000T+  T3:0000T-  T4:0000T-
 
```
- Le bloc `Id:` … ligne blanche se répète pour chaque spécimen. Après `Id:`, le nom est suivi d'espaces pour que `in:` commence toujours à la même colonne (nom + espaces = 13 caractères).
- Forme : `Cyl` (cylindre) ou `Cub` (cube), en dernier mot de la ligne `Id:`. Volume par défaut selon la forme : `v:10.20` pour un cylindre, `v:08.00` pour un cube. Si l'utilisateur ne donne pas la forme, demande-la avant d'écrire le fichier.
- Autres valeurs non données : laisse celles du modèle (0 pour les angles, `Site :` vide). N'invente pas d'orientation.
- « Sans mesure » : aucune ligne après `CompDes:`. Avec des mesures, une ligne par étape : étape, X, Y, Z, qualité, magnétomètre, susceptibilité (ex. `    0N0  3.292E-06  3.697E-05 -4.413E-05   4 M  837`).
- Structure en place (AMP) : ligne 6 « Prélèvements sur structure en place », vraies latitude, longitude et IGRF, et `Orient:SO  J:<jj>  M:<mm>  A:<aaaa>  H: <hh>  M:<mn>  S:<ss>  SM:<angle>` à la place de `Repère:`.
- Après le bloc, dis seulement : enregistrer sous le nom `<nom>.AMD` ; le logiciel du magnétomètre attend le codage Mac Roman (convertir le texte copié, qui est en UTF-8, si les accents posent problème).
