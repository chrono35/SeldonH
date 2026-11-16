# Pourquoi SeldonH

## 1. Rendre ChronoModel accessible

Au départ, il s'agissait d'aider les archéologues qui ne sont pas habitués aux outils mathématiques à utiliser ChronoModel.

L'arrivée des critères de convergence dans ChronoModel a rendu ce besoin urgent : il fallait une solution simple pour que les utilisateurs interprètent correctement ces valeurs. Notre première idée, naïve, était de donner à une IA générative le log a posteriori exporté par ChronoModel.

## 2. Premier constat : il faut encadrer le raisonnement du modèle

Nous avons vite compris qu'il fallait « encadrer » le raisonnement du modèle. Il commentait les résultats sans comprendre les principes théoriques du calcul. Par exemple, il pouvait proposer de supprimer une date dont les critères de convergence étaient insuffisants, alors qu'un défaut de convergence se règle par les réglages du MCMC et non en retirant des données.

C'est pour cela que SeldonH s'appuie sur des **compétences métier** : des fiches qui donnent au modèle les règles du domaine (stratigraphie, relations entre dates, lecture des diagnostics de convergence, etc.). Chaque utilisateur peut en ajouter pour enrichir l'outil avec sa propre expertise.

## 3. Second constat : un modèle de langage ne calcule pas

Un modèle de langage ne fait pas de calcul, il prédit du texte. Il ne faut donc pas lui confier un calcul.

Prenons un exemple. Si on demande « calcule la racine carrée de 36 », on peut lui faire confiance : tout modèle, même petit, a vu ce résultat des milliers de fois. « √36 = 6 » relève de la mémoire, pas du calcul. Mais pour √2,37, 3 847 × 1,0283 ou une propagation d'erreur, il donne un résultat plausible, souvent faux dès la 2ᵉ ou la 3ᵉ décimale, et sans le signaler. Les petits modèles locaux se trompent nettement plus.

Nous en avons conclu que la bonne solution est de donner au modèle des **outils de calcul** qui s'exécutent sur la machine de l'utilisateur. Ce n'est pas le modèle qui choisit librement l'outil. SeldonH le guide par un arbre de décision : il lui pose des questions fermées (« parle-t-on d'une courbe ? », « y a-t-il une stratigraphie ? ») et chaque réponse oriente vers l'outil adapté. Le modèle se contente de reformuler la demande et d'en extraire les valeurs, l'outil fait le calcul.

De même, un modèle de langage ne sait pas dessiner. Des outils dédiés tracent donc les courbes, en particulier les densités de probabilité.

Conséquence directe : **chaque nombre affiché par SeldonH doit avoir une provenance identifiable**. Soit il vient d'un outil, soit il vient des données de l'utilisateur. Il ne doit jamais être produit par le modèle lui-même. L'utilisateur peut ainsi vérifier d'où vient chaque valeur.

## 4. Le rôle de SeldonH aujourd'hui

SeldonH se veut un intermédiaire et un assistant (*helper*) entre l'utilisateur et ChronoModel. Il traduit le langage naturel en requêtes machine, sans que l'utilisateur ait à écrire de code de calcul. Concrètement, il permet de :

- faire des calculs simples : calibrer une date, combiner des dates, calculer un Event au sens de ChronoModel ;
- concevoir un modèle chronologique sans maîtriser ChronoModel, à partir d'une description du problème archéologique ou géologique, et produire le fichier `.chr` correspondant ;
- interroger les résultats obtenus, par exemple en faisant du post-traitement, et aider à les interpréter (diagnostics de convergence compris) ;
- exporter les graphiques, notamment en SVG, pour les publications.

SeldonH ne remplace pas le jugement de l'archéologue. Il propose, explique et calcule, mais les choix de modélisation (contraintes stratigraphiques, regroupements en phases, rejet d'une date) restent les décisions de l'utilisateur.

## 5. Reproductibilité et rigueur scientifique

Un modèle de langage ne répond pas deux fois exactement de la même façon. La même question peut conduire à un autre modèle chronologique, avec d'autres contraintes ou d'autres regroupements. Un résultat obtenu avec SeldonH n'est donc scientifiquement recevable que si l'on peut retrouver comment il a été produit.

C'est pourquoi **la conversation doit être sauvegardée**, au même titre que les données et le fichier `.chr`. Elle conserve :

- les questions posées par l'utilisateur, mot pour mot ;
- les réponses du modèle et les décisions prises au fil de l'échange ;
- les calculs réalisés par les outils et leurs résultats ;
- les graphiques produits.

Pour chaque question, SeldonH enregistre aussi le prompt complet envoyé au modèle, avec la date, la version de SeldonH, le nom du modèle de langage, ses réglages (température, graine aléatoire, etc.), les compétences métier utilisées et le chemin suivi dans l'arbre de décision. Avec ces éléments, un relecteur ou un collègue peut suivre tout le raisonnement, refaire les calculs et vérifier que le modèle `.chr` correspond bien à la description du site. La conversation joue le rôle d'un cahier de laboratoire.

## 6. Le choix de modèles locaux

Utiliser un modèle hébergé à l'étranger pose des problèmes de **souveraineté et de confidentialité des données**, par exemple pour des données de fouille non publiées. Il faut aussi pouvoir travailler **hors ligne**, par exemple sur le terrain quand il n'y a pas de réseau. Ces deux contraintes poussent à utiliser de petits modèles installés sur l'ordinateur de l'utilisateur.

Cette contrainte rejoint les deux constats précédents. Un petit modèle local est moins fiable en calcul et en raisonnement, et ce sont justement les outils et les compétences métier qui compensent ces faiblesses. SeldonH peut aussi se connecter à un modèle distant quand l'utilisateur le souhaite et que les conditions le permettent.

## 7. Ce que SeldonH ne sait pas encore faire

SeldonH est en cours de développement et n'a pas encore été diffusé. Plusieurs points restent ouverts :

- il ne lance pas lui-même le calcul MCMC : l'utilisateur ouvre le fichier `.chr` dans ChronoModel, lance le calcul, puis rapporte les résultats dans SeldonH ;
- l'import dans ChronoModel de certains types de dates produits par SeldonH (radiocarbone, F14C, densités tabulées) reste à valider ;
- un petit modèle local peut encore mal comprendre une description de site complexe ou ambiguë. SeldonH doit alors poser des questions plutôt que deviner ;
- les compétences métier couvrent encore une partie seulement des situations rencontrées en archéologie et en géologie, et doivent être enrichies par les utilisateurs.

## Pourquoi « Seldon » ?

Le nom vient de Hari Seldon, le mathématicien du cycle *Fondation* d'Isaac Asimov. Seldon invente la psychohistoire, une science qui prévoit statistiquement l'avenir de grandes populations. SeldonH fait le chemin inverse : il aide à reconstruire le passé par la statistique bayésienne. Le « H » signifie *Helper*.

Le clin d'œil va plus loin. Dans ses derniers romans, Asimov relie le cycle des Robots à celui de Fondation. Dans *Prélude à Fondation* (1988), on découvre que le robot R. Daneel Olivaw, caché à la fois sous les traits du journaliste Chetter Hummin et sous ceux du Premier ministre de l'Empereur, Eto Demerzel, a protégé Seldon et l'a poussé à rendre la psychohistoire utilisable. Daneel obéit aux trois lois de la robotique, auxquelles il a ajouté lui-même une loi « zéro » qui place la protection de l'humanité au-dessus de tout. SeldonH reprend l'idée d'une machine au service de l'humain, encadrée par des règles : il ne doit jamais inventer un résultat. Mais il s'en distingue sur un point : là où Daneel décide en secret à la place des hommes, SeldonH explique ce qu'il fait, et la décision scientifique revient toujours à l'archéologue.
