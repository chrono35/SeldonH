---
name: Radiocarbone et calibration
description: Âge 14C conventionnel, courbes de calibration, effet réservoir, ΔR
keywords: radiocarbone, 14c, c14, carbone 14, calibration, calibrer, calibre, intcal, intcal20, shcal, marine, marine20, reservoir, delta r, bp conventionnel, laboratoire, radiocarbon, carbon 14, carbon-14, calibrate, calibrated, reservoir effect, conventional age, laboratory, radiocarbon date
priority: 5
---
- L'âge radiocarbone conventionnel (BP, ± σ) est calculé avec la demi-vie de Libby (5568 ans) et le présent = 1950 ; il n'est **pas** un âge calendaire.
- Il se convertit en âge calendaire par **calibration** avec une courbe : IntCal20 (hémisphère Nord, matériel terrestre), SHCal20 (hémisphère Sud), Marine20 (matériel marin, avec correction régionale **ΔR**).
- La calibration produit souvent une distribution **multimodale** (plusieurs intervalles) à cause des plateaux et des oscillations de la courbe : ne jamais résumer par une seule valeur.
- Dans ChronoModel, on saisit l'âge 14C et son erreur dans le plugin 14C, on choisit la courbe (et ΔR ± erreur si milieu marin/réservoir) : la calibration est faite par le logiciel dans le modèle.
- Courbes disponibles : IntCal20, IntCal13, IntCal09, IntCal04 ; SHCal20, SHCal13, SHCal04 ; Marine20, Marine13, Marine09, Marine04 ; par défaut IntCal20 (SHCal20 si hémisphère sud, Marine20 si matériel marin). Pour un échantillon postérieur à 1950 (bombe), utiliser la méthode F14C.
- **SeldonH calibre lui-même** quand la question contient un âge avec son erreur et un contexte 14C (ex. « C14 de 2500 ± 30 BP », « coquille marine 3000 ± 30 BP, ΔR 100 ± 50 », « hémisphère sud »). Le résultat exact (intervalles HPD à 95 % et 68 %, médiane) arrive dans un bloc « [Résultat de calcul de SeldonH] » : reprends-le tel quel, explique-le (multimodalité, plateau de la courbe, précision), ne le recalcule pas.
- Un âge conventionnel est toujours en **BP** : 1954 ± 50 BP n'a rien d'incohérent (≈ 2 000 ans, soit vers le début de notre ère). Ne le confonds pas avec une année calendaire.
- Sans bloc de résultat (âge ou erreur manquants), ne cite aucun intervalle calibré de mémoire : demande l'âge ± erreur, la courbe (ou le matériau/l'hémisphère) et le ΔR si le matériel est marin.
- Dans un modèle ChronoModel, la calibration finale reste faite par ChronoModel ; le calcul de SeldonH sert à une première estimation d'une date isolée.
- Informations utiles à demander : numéro de laboratoire, matériau (charbon, os, coquille…), courbe, ΔR, contexte stratigraphique.
