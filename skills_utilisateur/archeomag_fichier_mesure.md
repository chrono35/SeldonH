---
name: Lire un fichier de mesure archéomagnétique (AMD / AMP)
description: Structure d'un fichier de mesure du magnétomètre (Spinner Molspin ou cryogénique) : en-tête, spécimens, paliers thermiques, anisotropie, susceptibilité
keywords: fichier amd, fichier amp, format amd, format amp, armag, archeomag, molspin, spinner, magnetometre, cryogenique, fichier de mesure, fichier de mesures, mesure archeomagnetique, mesures archeomagnetiques, specimen, specimens, paliers thermiques, palier thermique, desaimantation, aimantation, susceptibilite, anisotropie, materiaux deplaces, structure en place, orientation solaire, measurement file, demagnetization, susceptibility, anisotropy
priority: 6
---
Un fichier de mesure archéomagnétique est un texte en colonnes. `.AMD` = prélèvements sur matériaux déplacés (non orientés) ; `.AMP` = prélèvements sur structure en place (orientés au soleil). Explique la structure en citant les lignes du fichier joint ; n'invente aucune valeur et ne calcule ni direction ni date à partir de ces mesures (SeldonH n'a pas d'outil pour cela).

**En-tête** (8 lignes) : appareil (« Spinner_Molspin 2008 »), Commune, Site, Latitude et Longitude en degrés minutes secondes, `IGRF:` = déclinaison du champ au site en degrés, type de prélèvement (déplacés / en place), type de carottage, date de création. En AMD, latitude, longitude et IGRF à 0 sont normaux.

**Bloc d'un spécimen** :
- Ligne `Id:` : nom du spécimen (AMD : `14D` ; AMP : `3P2` = prélèvement 3, spécimen 2) ; `in` inclinaison et `az` azimut du prélèvement (°) ; `Tet`, `Psy` angles d'orientation complémentaires ; `v` volume (cm³) ; `com:TH60.0°T` = traitement thermique avec champ de laboratoire de 60 µT (`TH 0.0` : pas de champ) ; dernier mot : forme `Cub` (cube, volume habituel 8,00 cm³) ou `Cyl` (cylindre, 10,20 cm³).
- Ligne `Orient:SO` (AMP) : orientation au soleil, date J M A, heure H M S, `SM` angle solaire relevé sur le terrain. Ligne `Repère:` vide (AMD) : pas d'orientation.
- Ligne `CompDes:` : bornes des composantes choisies, T1–T2 pour la composante 1, T3–T4 pour la composante 2 (température sur 4 chiffres, `0000` = non choisie).

**Lignes de mesure** (7 colonnes) : étape, X, Y, Z, qualité, magnétomètre, susceptibilité.
- Étape : `0N0` = aimantation naturelle (avant chauffe) ; `350T+` / `350T-` = mesure après chauffe à 350 °C, spécimen placé dans le four dans un sens (+) ou dans l'autre (−) ; `450Z+ Z- X+ X- Y+ Y-` = aimantations acquises selon les 6 directions pour l'anisotropie, `450ZB` = remesure de contrôle de Z+ (une forte différence signale une altération) ; un `?` dans le nom (`450Y?`) = mesure douteuse, à signaler.
- X Y Z : moment magnétique en A·m² dans le repère du spécimen ; aimantation (A/m) = moment / volume (cm³ × 10⁻⁶).
- Qualité de la mesure : son échelle dépend du magnétomètre ; ne compare que des mesures du même appareil.
- Magnétomètre : `M` = Molspin, `C` = cryogénique.
- Susceptibilité : mesurée seulement à certaines étapes ; 0 = non mesurée (pas une valeur nulle).

**Accents** : les fichiers d'origine sont en codage Mac Roman. Si les accents apparaissent comme « � », dis-le : les noms propres touchés (« Coss� ») sont à corriger par l'utilisateur ; conseille de convertir le fichier en UTF-8 avant de le joindre.
