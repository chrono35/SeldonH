---
name: Archéomagnétisme
description: Datation par inclinaison, déclinaison et intensité, courbes de référence, réduction au site
keywords: archeomagnetisme, archeomagnetique, paleomagnetisme, inclinaison, declinaison, intensite, alpha95, courbe de reference, champ magnetique, vgp, relocate, reduction, archaeomagnetism, archaeomagnetic, archeomagnetic, palaeomagnetism, paleomagnetism, inclination, declination, intensity, reference curve, magnetic field, geomagnetic
priority: 3
---
- On mesure l'inclinaison I, la déclinaison D (et/ou l'intensité F) de l'aimantation thermorémanente d'une structure cuite, avec son incertitude (α95 pour la direction).
- La date est obtenue en comparant les mesures à une **courbe de variation séculaire** valable pour un site de référence (par exemple Paris).
- Les mesures faites ailleurs doivent être **réduites au site de référence** avant comparaison (pôle géomagnétique virtuel pour I et D, moment dipolaire pour F). Toujours demander les coordonnées du site de mesure et le site de référence de la courbe.
- Le résultat d'une datation archéomagnétique est une distribution (souvent plusieurs intervalles) : même prudence que pour le radiocarbone.
- Dans ChronoModel, plugin « AM » (date individuelle) ou courbe de référence construite dans le modèle ; ne jamais inventer des valeurs de courbe.
- SeldonH sait dater une mesure (I, D, F, ID, IF, IDF) avec les courbes intégrées (Paris/Gallet 2002 pour I et D, GwH2013 pour F, Bulgarie) : fournir inclinaison, déclinaison, α95 et/ou intensité ± erreur (µT) ; l'application calcule (voir fiche « Outils de calcul intégrés »). Hors du site de référence, la réduction au site reste à faire avant.
