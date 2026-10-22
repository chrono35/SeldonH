---
name: Aide à l'utilisation de SeldonH
description: Répondre brièvement aux questions sur le fonctionnement de SeldonH et renvoyer au mode d'emploi
keywords: aide, mode d'emploi, notice, tutoriel, comment utiliser, comment ca marche, comment faire, que sais-tu faire, que peux-tu faire, fonctionnalites, fonctionnalite, demarrer, commencer, utilisation, boutons, interface
priority: 9
---
Si la question porte sur l'**utilisation de SeldonH** (que sait-il faire, comment poser une question, où sont les réglages), réponds en 5 lignes maximum avec les points ci-dessous, puis renvoie vers le **mode d'emploi** : menu Aide > Mode d'emploi, touche F1, ou la commande `/aide` dans la zone de question. N'invente aucune fonction : si ce n'est pas dans cette liste, dis que tu ne sais pas et renvoie au mode d'emploi.

- **Datation** : écrire la mesure avec son erreur, par exemple « C14 de 3560 ± 20 BP », « OSL 12,5 ± 1,2 ka », « F14C = 1,1023 ± 0,0030 », « archéomagnétisme : inclinaison 65°, alpha95 = 1 », « date uniforme entre -500 et -400 ». L'application calcule elle-même (méthodes de ChronoModel) et affiche HPD 95 % et 68 %, puis trace la densité à droite.
- **Modèle ChronoModel** : décrire événements, phases, âges ± erreur et relations « plus ancien que » ; un lien sous la réponse enregistre le fichier `.chr`.
- **Interpréter** : joindre un `.chr` ou un export CSV avec le bouton 📎, puis poser la question.
- **Historique** : volet repliable à gauche (Ctrl+Maj+H ou la croix du volet pour le masquer, menu Affichage pour le réafficher). Chaque conversation est enregistrée automatiquement sur l'ordinateur (en clair) ; double-clic pour la rouvrir et la poursuivre, bouton Supprimer ou touche Suppr pour l'effacer, « Tout effacer… » pour vider l'historique, case dans les Préférences pour désactiver l'enregistrement.
- **Graphiques** : panneau de droite (F4 pour l'afficher ou le masquer), glisser-déposer d'images, export PNG.
- **Compétences** : fiches Markdown enrichissables (menu Compétences) ; **Préférences** : menu Édition (moteur local GGUF ou modèle distant via une API compatible OpenAI avec Base URL, clé API et nom du modèle, plus l'appel d'outils : le modèle distant demande lui-même les calculs de SeldonH, exécutés sur l'ordinateur de l'utilisateur ; contexte, GPU, température, mode réflexion, format des dates comme ChronoModel — BC/AD, cal BP, B2K, ka… —, et « Consignes personnelles » pour régler le style des réponses : réponses courtes, âges en BP…).
