                 		Evil Islands
             		   Curse of the lost soul
===============================================================================
                 		Février 2001


Copyright 2000 Nival Interactive,
Copyright 2000 Ravensburger Interactive Media GmbH
Tous droits réservés

Fishtank Interactive est une marque commerciale déposée de 
Ravensburger Interactive Media GmbH

Table des matičres du fichier Readme
====================================
1. Configuration systčme
2. Conseils de configuration du jeu
3. FAQ Multijoueur
4. Dépannage
5. Support technique


===============================================================================
1. Configuration systčme
========================
Configuration minimum :
	Windows 95/98/2000, DirectX 7.0
	Pentium II 233 MHz
	64 Mo de RAM
	Carte graphique accélératrice 3D AGP
	8 Mo de RAM vidéo
	Carte sonore

Vous devez disposer de 550 Mo  d'espace non compressé sur votre disque dur pour 
installer ce jeu. De plus, pour assurer la stabilité  du jeu, il  est vivement 
conseillé de disposer de 300 Mo d'espace non compressé supplémentaire.


Configuration recommandée :
	Windows 95/98/2000, DirectX 7.0
	Pentium III 450 MHz
	128 Mo de RAM
	Carte graphique accélératrice 4x 3D AGP 
	32 Mo de RAM vidéo
	Carte sonore

Remarque : il est conseillé d'exécuter ce jeu sous Windows 98.


===============================================================================
2. Conseils de configuration du jeu 
===================================

2.1 Section " Options " du programme d'exécution automatique - 
paramčtres

a) Qualité du terrain

   Faible - active des dégradés des détails ŕ 55 m et 90 m. Aucun effet d'onde. 
      La qualité visuelle sera bonne.
   Moyenne - active des dégradés des détails ŕ 90 m et 120 m. Aucun effet 
      d'onde. La qualité visuelle sera trčs bonne.
   Elevée - active des dégradés des détails ŕ 90 m et 120 m. Effets d'onde. 
      La qualité visuelle sera trčs bonne.

Augmentation des FPS : 
Elevée => Moyenne ~70 %, Moyen => Faible ~20 %
Les valeurs Elevée et Moyenne n'engendrent presque pas de différence visuelle, 
mais les fps sont réduits d'environ ~1.5 ŕ 2 fois.

b) Fréquence d'éclairage
   Rare - une fois par minute. La qualité visuelle sera faible, 
      mais le temps d'accčs au processeur est pratiquement nul.
   Moyenne - une fois toutes les 10 secondes. Meilleure qualité 
      que Rare, mais certains artéfacts seront toujours visibles. 
   Fréquente - une fois par seconde. Meilleure qualité possible.

Augmentation des FPS : 
Fréquente => Moyenne ~20-30 %, Moyenne => Rare ~10-15 %

c) Qualité des textures
   Faible - mauvaise qualité de textures, mais exploitation 
      minimale de la mémoire vidéo.
   Moyenne - qualité de textures moyenne, mais exploitation de la 
      mémoire vidéo 3 ŕ 4 fois inférieure. Cette option est 
      appropriée pour les cartes vidéo disposant de 8 Mo de RAM 
      ou plus.
   Elevée - meilleure qualité de textures possible, mais exploitation
      maximale de la mémoire vidéo. Cette option est appropriée pour 
      les cartes vidéo disposant de 16 Mo de RAM ou plus (32 Mo 
      recommandés).

Si la qualité des textures choisie est la qualité recommandée, le paramčtre 
n'aura pratiquement aucun effet sur les performances de la machine (environ 5 % 
de différence), car toutes les textures sont chargées dans la mémoire vidéo de 
toutes façons. Cependant, si la qualité des textures choisie est supérieure ŕ 
celle recommandée, les performances seront diminuées. En effet, les textures 
seront alors chargées par le biais du bus AGP, qui était réservé ŕ la carte 
vidéo (environ 10 % de pertes de performances) ou ŕ partir de la mémoire systčme 
(les fps seront alors 4 fois inférieurs).

d) Filtrage
   Point - faible qualité d'image
   Bilinéaire - bonne qualité d'image
   Trilinéaire - excellente qualité d'image

Les  différences en termes de fps entre Point et Bilinéaire sont presque 
négligeables. En revanche, les fps sont réduits d'environ 30 % entre Bilinéaire 
et Trilinéaire.

e) Mipmapping
Lorsque cette option est désactivée, elle permet d'économiser environ
25 % de la mémoire vidéo, mais elle réduit sensiblement la qualité de 
l'image et les performances en général.

f) Juxtaposition
Cette option améliore la qualité de l'image en mode Couleurs 16 bits. 
Aucune diminution des performances n'a été constatée.

g) Etirer vidéos
Cette option risque de saccader les vidéos avec de nombreuses cartes vidéo... 
La meilleure méthode consiste ŕ essayer sur votre machine. Ce paramčtre n'a 
aucun effet sur les performances dans les autres écrans de jeu.

h) Curseur indépendant des fps
Cette option diminue les performances d'environ 30 %, mais assure un 
rafraîchissement du curseur de l'ordre de 16 fois par seconde, indépendamment 
des fps. Cette option est recommandée pour les machines les moins puissantes.

i) Nombre de couleurs
   16 bits - vitesse correcte et qualité acceptable
   32 bits - avec la plupart des cartes vidéo (sauf la carte ATI 
      Rage 128), les fps sont diminués d'environ 2 fois, mais la 
      qualité de l'image est sensiblement améliorée. 


2.2 Paramčtres " Options " du jeu n'influant pas sur la vitesse

Nous vous recommandons de ne laisser que les ombres des personnages, car les 
ombres des objets et des arbres exploitent beaucoup de mémoire vidéo (et peut 
réduire le temps d'accčs au processeur, selon la Fréquence d'éclairage choisie).


2.3 Paramčtres du fichier d'échange de Windows

Pour assurer la fluidité du jeu, nous vous recommandons de laisser Windows gérer 
la taille du fichier d'échange automatiquement, et de ne pas la configurer vous-
męme. Lorsque le jeu s'exécute, il vérifie la quantité d'espace disque requise 
pour une exécution normale. Si un message d'avertissement s'affiche ŕ l'écran, 
vous devrez libérer de l'espace sur votre disque dur. L'espace minimum requis 
sur votre disque dur est d'environ 300 Mo, mais nous vous recommandons de 
libérer environ 500 Mo. Vérifiez également la défragmentation de votre disque: 
l'optimisation de ce paramčtre peut sensiblement augmenter la vitesse du jeu, 
ainsi que la vitesse de chargement des Zones de jeu. Lancez l'utilitaire 
Défragmenteur de disque aprčs avoir installé le jeu. Nous vous recommandons 
fortement d'installer le jeu sur un disque (ou une section de disque) non 
compressé.


===============================================================================
3. FAQ Multijoueur
==================

Q : De quoi ai-je besoin pour jouer via Internet ?
R : Tout d'abord, vous devez  disposer d'une connexion ŕ Internet. La  meilleure 
solution, en particulier si vous créez votre propre serveur, est d'utiliser  une 
connexion avec ligne dédiée, bien que peu de personnes aient accčs ŕ ce genre de 
connexion pour le moment. Avec  une connexion permanente ŕ Internet, la machine 
dispose généralement d'une adresse IP statique, ce qui représente une  meilleure 
option pour un serveur. Pour jouer en tant que client, une connexion ŕ  distance 
d'au moins  28 Kb/s  est suffisante.  Cependant, un  modem ŕ  grande vitesse (56 
Kb/s) ou une ligne ISDN (64 ou 128 Kb/s) améliorerait grandement les choses.  Si 
vous souhaitez créer votre propre serveur, vous devez disposer d'un accčs direct 
ŕ Internet.

Q : Est-il possible d'utiliser une connexion ŕ Internet par satellite ?
R : Les connexions ŕ Internet par satellite utilisent généralement des 
configurations avec serveur proxy pour prendre en charge les protocoles http et 
ftp. Il est impossible d'exécuter le jeu par le biais d'un serveur proxy. Męme 
si vous disposez d'un accčs direct ŕ Internet, les connexions par satellite ne 
traitent les signaux qu'aprčs un certain laps de temps, ce qui rend difficile 
les actions jointes ŕ plusieurs joueurs.

Q : Comment puis-je connaître mon adresse IP ?
R : Ce type de question nous est généralement posé par des utilisateurs de 
connexion ŕ distance, qui se voient attribuer différentes adresses IP dynamiques 
chaque fois qu'ils se connectent ŕ leur fournisseur d'accčs. Exécutez le 
programme winipcfg.exe (situé ŕ la racine de votre répertoire Windows). Le 
programme affichera la configuration réseau actuelle de votre machine, et par 
conséquent son adresse IP. Si vous ętes connecté ŕ plusieurs réseaux (LAN et 
connexion ŕ distance, par exemple), assurez-vous que les listes déroulantes des 
pilotes contiennent bien le pilote avec lequel vous ętes connecté ŕ Internet. Si 
vous utilisez un programme de numérotation automatique ŕ distance (nous vous 
recommandons fortement d'en utiliser un), l'adresse IP s'affichera sans doute 
une fois la connexion effectuée.

Q : Comment savoir si j'ai un accčs direct ŕ Internet ?
R : Si vous ne vous connectez ŕ Internet que par le biais d'un serveur proxy, 
votre administrateur réseau devrait vous indiquer que la connexion s'est bien 
effectuée et vous invite ŕ configurer votre explorateur en conséquence (par 
exemple, IP_proxy:3128). Si en revanche vous pouvez vous connecter sans passer 
par un serveur proxy, alors vous disposez sans doute d'un accčs direct ŕ 
Internet. Cependant, si vous vous connectez par le biais d'un NAT, vous pourriez 
croire que vous disposez d'un accčs direct ŕ Internet, alors qu'en réalité, 
votre ordinateur ne sera pas accessible par les autres ordinateurs en WAN. Pour 
savoir si vous disposez d'un accčs direct ŕ Internet, vérifiez votre adresse IP 
(voir question précédente).
Les LAN disposent d'une fourchette d'adresses qui leur est réservée :

10.0.0.0    - 10.255.255.255
169.254.0.0 - 169.254.255.255
172.16.0.0  - 172.31.255.255
192.168.0.0 - 192.168.255.255

Si votre adresse IP se trouve dans l'une de ces fourchettes, vous ne disposez 
pas d'un accčs direct ŕ Internet et vous ne pourrez sans doute pas configurer de 
serveur pour Evil Islands. Les autres clients ne pourront se connecter ŕ votre 
serveur que s'ils sont reliés ŕ votre LAN. Si vous vous connectez par le biais 
d'un NAT, vous pourrez probablement jouer en tant que client sur d'autres 
serveurs Internet.

Q : J'ai une adresse IP dynamique. Puis-je créer mon propre serveur ?
R : Oui, c'est possible. Il sera accessible par les joueurs qui en connaîtront 
l'adresse IP ou ŕ ceux qui reçoivent les informations du serveur maître (si vous 
avez décidé de publier ces informations sur le serveur maître). En revanche, les 
autres joueurs ne pourront pas garder votre adresse dans leur carnet d'adresses. 
Ou plutôt, ils peuvent, mais l'adresse en question ne leur garantit pas de 
pouvoir se connecter ŕ votre serveur la prochaine fois. Certains fournisseurs 
d'accčs réservent l'adresse IP de leurs clients pendant un certain temps suivant 
leur déconnexion (en général 10 ŕ 15 minutes). Dans ce cas, si la connexion est 
perdue et que vous vous connectez ŕ nouveau (avec un programme de connexion ŕ 
distance automatique, par exemple), votre machine recevra la męme adresse IP et 
vos clients pourront récupérer leur connexion plus facilement. Si vous n'avez 
pas ce service ŕ votre disposition, vous devrez attendre environ une minute, 
temps nécessaire au serveur maître pour supprimer les informations relatives ŕ 
votre serveur. 

Q : Comment puis-je me connecter au serveur par le biais d'un modem ?
R : Si votre serveur est connecté ŕ Internet par le biais d'un modem ordinaire, 
ne configurez pas le nombre maximum de joueurs sur 6. Si vous le faites, vos 
clients, ainsi que vous-męme, souffriront d'une vitesse de jeu trčs faible, avec 
beaucoup de ralentissements et de saccades. Configurez plutôt le nombre maximum 
de joueurs selon la vitesse réelle de la connexion fournie par votre ligne 
téléphonique. Notez bien que les clients ont bien plus de trafic en entrée qu'en 
sortie. Le serveur doit donc pouvoir prendre en charge des volumes importants en 
sortie. L'utilisation d'un modem avec un protocole asymétrique de 56 ?b/s (V90, 
56K) risque d'ętre inefficace. Il est donc conseillé d'utiliser une connexion de 
33,6 ?b/s avec un protocole symétrique ordinaire. En revanche, le protocole 
asymétrique fonctionnera mieux chez les clients.

Q : Est-il possible de jouer par le biais d'un serveur proxy ?
R : Cela dépend. Ce qui est certain, c'est que vous ne pourrez pas créer votre 
propre serveur. Pour vous connecter ŕ d'autres serveurs de jeu, votre serveur 
proxy doit permettre un trafic bidirectionnel de paquets UDP sur le port 8888, 
et pour vous connecter au serveur maître, des paquets TCP sur le port d'adresse 
28006. La meilleure option est de vous connecter par le biais d'un NAT 
permettant toutes sortes de trafic de paquets. Si ce n'est pas le cas, contactez 
votre administrateur réseau pour configurer votre serveur proxy correctement.

Q : Pourquoi mon serveur de jeu ne se trouve-t-il pas dans la liste du serveur 
    maître ?
R : Vous devez d'abord vous assurer que la case " Closed server " n'est pas 
cochée. Vous devez disposer d'une bonne connexion avec le serveur maître. 
Actuellement, le serveur maître est situé dans nos locaux. Vérifiez donc votre 
connexion avec Nival en tapant (dans une fenętre MS-DOS) :

ping server.nival.com

Si votre connexion est bonne, alors le problčme réside sans doute dans l'un des 
routeurs entre votre machine et le serveur maître (de votre côté), qui bloque 
les paquets TCP au port d'adresse 28006. Contactez votre administrateur réseau 
pour résoudre ce problčme.

Q : Mon serveur de jeu se trouve dans la liste, mais personne ne peut s'y 
    connecter. Pourquoi ?
R : Probablement parce que les utilisateurs ne peuvent pas établir de connexion 
directe avec votre machine. Si vous lancez votre serveur ŕ partir d'un LAN 
séparé de l'Internet par un serveur proxy ou un NAT, il arrive dans certains cas 
(rares) que votre serveur soit capable de se connecter au serveur maître, alors 
que les autres joueurs ne peuvent pas accéder ŕ votre serveur. Ils verront le 
nom de votre serveur dans la liste, mais la valeur 9999 sera indiquée dans la 
colonne Ping. Il est également possible que l'un des routeurs de votre côté 
bloque des paquets UDP (tous les paquets ou juste ceux envoyés au port d'adresse 
8888). Par conséquent, les autres joueurs verront votre serveur dans la liste, 
mais ne pourront pas s'y connecter, et vous ne pourrez pas non plus jouer sur 
leurs serveurs. Contactez votre administrateur réseau pour résoudre le problčme 
de trafic des paquets UDP.

Q : J'ai téléchargé la liste du serveur maître et certains serveurs de jeu 
    indiquent la valeur 9999 dans la colonne Ping. Pourquoi ?
R : Il est possible que ces serveurs se soient déconnectés peu de temps 
auparavant, mais n'aient pas encore été effacés de la liste. Attendez encore 
quelques minutes et réactualisez la liste. Si ces serveurs sont encore lŕ, 
consultez la réponse ŕ la question précédente.


===============================================================================
4. Dépannage
============

En cas de mauvaise visualisation, nous vous conseillons d'utiliser les pilotes 
situés dans le dossier \Drivers du CD:

	ATI
	    Rage 3D, Rage Pro, etc (legacy HW)
	    Rage 128, Rage 128 Pro
	    Radeon
	3DFX
	    Voodoo III
	    Voodoo IV
	    Voodoo V
	nVidia
	    Riva128
	    TNT
	    TNT2
	    GeFORCE256
	    GeFORCE2

Adaptateurs vidéo Intel i740
============================
Les pilotes officiels d'Intel pour les adaptateurs i740 ne sont pas totalement 
compatibles avec DirectX 7.0, ce qui a pour effet d'afficher une erreur  lorsque 
vous entrez dans une Zone de jeu, lorsqu'une quantité importante de textures a 
été chargée dans la mémoire vidéo. Cependant, cette incompatibilité semble avoir 
été résolue dans la version 8.0 de DirectX. Sous Windows 98 et Windows 2000, le 
jeu peut ętre exécuté sans problčmes (en mode d'affichage 640x480x16), mais nous 
ne recommandons pas l'utilisation de cet adaptateur ŕ cause de ses limitations 
en couleurs 16 bits, de son anti-crénelage et de ses performances vidéo. Nous ne 
pouvons pas non plus garantir la compatibilité d'un i740 avec toutes les 
versions de Windows (contrairement aux adaptateurs cités dans la liste 
recommandée).

Exécution d'applications en co-habitation
=========================================
Afin d'assurer une utilisation stable du jeu, nous vous recommandons fortement 
de ne pas utiliser le raccourci-clavier Alt+Tab pour passer d'une application ŕ 
une autre. En outre, nous vous recommandons de fermer toutes les applications 
ouvertes avant d'exécuter le jeu (en particulier Microsoft Outlook).


===============================================================================
5. Support technique
====================

N'hésitez pas ŕ envoyer vos questions, commentaires et suggestions ŕ Fishtank 
Interactive par courrier électronique ŕ l'adresse :

support@fishtank-interactive.com

Nos sites Web
=============
www.evil-islands.com
www.fishtank-interactive.com
www.ravensburger.de/interactive
www.nival.com

AMUSEZ-VOUS BIEN !
