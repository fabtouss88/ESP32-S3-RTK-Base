# ESP32-S3-RTK-Base
Firmware FreeRTOS dual-core pour ESP32-S3 et Quectel LC29HEA. Crée une station de base RTK géo-référencée permanente (NVS). Diffuse un flux RTCM3 local (port 2101) et un stream NTRIP Source asynchrone non-bloquant vers le réseau Onocoy. Interface Web avec télémétrie et console de logs.
🛰️ ESP32-S3 & Quectel LC29HEA - Station de Base RTK Multi-Réseaux

Ce dépôt héberge le code source d'un firmware industriel et hautement optimisé, basé sur une architecture multi-tâches FreeRTOS Dual-Core sur ESP32-S3. Il permet de transformer un microcontrôleur ESP32-S3 couplé à un récepteur GNSS bi-fréquence Quectel LC29HEA (L1/L5) en une station de base RTK géo-référencée permanente et autonome.

La station diffuse simultanément :

Un flux binaire local de corrections différentielles RTCM3 sur le port TCP 2101 (pour vos rovers, robots tondeuses, etc.).

Un flux asynchrone et non-bloquant (NTRIP Source) vers le réseau mondial de géolocalisation décentralisée Onocoy.

🛠️ 1. Guide de Compilation et Téléversement (Arduino IDE)

📋 Bibliothèques requises à ajouter

Le code s'appuie principalement sur les briques réseau et système natives d'Espressif (WiFi, WebServer, Preferences, vector). Une seule bibliothèque externe doit être installée via le gestionnaire de bibliothèques de l'IHM Arduino :

Adafruit NeoPixel : Requise pour le pilotage de la LED RVB d'état de l'ESP32-S3.

⚙️ Paramètres de configuration dans l'IHM Arduino IDE

Pour une carte de développement standard ESP32-S3 (type N16R8), appliquez scrupuleusement la configuration suivante dans le menu Outils :

Option de carte

Valeur à sélectionner

Rôle technique

Carte (Board)

ESP32S3 Dev Module

Cible l'architecture matérielle correcte.

USB CDC On Boot

Enabled

CRITIQUE : Force l'envoi de la console série sur le port USB natif de la puce. Évite que la carte ne disparaisse définitivement de Windows après le premier flashage.

Flash Size

16MB (128Mb)

Aligné sur la capacité physique des modules N16.

Partition Scheme

16MB Flash (2MB APP/12.5MB FATFS)

Réserve l'espace suffisant pour accueillir l'IHM Web embarquée.

Core Debug Level

None (ou Error)

Désactive les logs système superflus pour optimiser le processeur.

📲 2. Mises à jour à distance (OTA)

Pour éviter d'avoir à descendre la station de son mât sur le toit lors d'une mise à jour de firmware, deux protocoles de téléversement à distance (OTA) sont intégrés :

Méthode A : Téléversement réseau en direct depuis l'IHM Arduino IDE

Connectez votre ordinateur sur le même réseau local (Wi-Fi) que votre station de base RTK.

Dans l'IHM Arduino, ouvrez le sélecteur de ports : une section "Network Ports" (Ports réseau) apparaît automatiquement en affichant l'adresse IP de votre carte.

Sélectionnez ce port réseau et cliquez sur Téléverser. Le code est compilé et poussé à travers le réseau Wi-Fi.

Méthode B : Flashage manuel du fichier binaire compilé (.bin)

Dans l'IHM Arduino, générez le fichier compilé via le menu : Croquis ➡️ Exporter le binaire compilé. Un fichier .bin est créé dans le dossier de votre croquis.

Ouvrez votre navigateur et saisissez l'adresse de mise à jour dédiée de votre base : http://<IP_DE_VOTRE_BASE>/update.

Connectez-vous, puis glissez-déposez le fichier .bin généré. La carte écrit le programme sur sa partition passive, vérifie son intégrité, bascule et redémarre de manière autonome en moins de 10 secondes.

🖥️ 3. Flashage direct de fichiers .bin (Sans installation d'IDE)

Si vous fournissez le projet à un tiers ou souhaitez flasher une nouvelle carte sans installer l'IHM Arduino, vous pouvez utiliser un outil de flashage USB directement en ligne depuis votre navigateur internet (Chrome, Edge ou Opera requis pour l'accès aux ports COM via WebUSB) :

Rendez-vous sur le site officiel d'installation Espressif : Espressif Installeur Web (esptool-js) (ou l'alternative ESPHome Web Flasher).

Connectez l'ESP32-S3 à votre ordinateur avec un câble de données USB connecté sur son port natif.

Cliquez sur Connect et sélectionnez le port série associé (Ex: COM20).

Choisissez le fichier binaire de release précompilé BASERTK.ino.bin.

Renseignez obligatoirement l'adresse d'écriture offset de démarrage : 0x10000 (offset d'application standard sur ESP32-S3).

Cliquez sur Program ou Flash pour téléverser. La carte redémarre automatiquement une fois l'opération terminée.

🔌 4. Schéma de Câblage Matériel (Pinout UART)

L'ESP32-S3 alimente et communique avec la carte breakout du récepteur Quectel LC29HEA en utilisant un bus série croisé classique :

       ESP32-S3 DevKitC                      Quectel LC29HEA
    ┌────────────────────┐                ┌────────────────────┐
    │                    │                │                    │
    │         5V ────────┼────────────────┼► VCC (5V)          │
    │        GND ────────┼────────────────┼► GND               │
    │                    │                │                    │
    │  GPIO 17 (TX1) ────┼────────────────┼► RXD               │
    │  GPIO 16 (RX1) ◄───┼────────────────┼── TXD              │
    │                    │                │                    │
    └────────────────────┘                └────────────────────┘


⚠️ IMPORTANT - Alimentation électrique : La radio Wi-Fi de l'ESP32-S3 (lors de l'éjection de paquets) cumulée à l'amplificateur actif de l'antenne GNSS K700 engendre des pics de courant transitoires pouvant dépasser 500 mA. Utilisez impérativement un bloc secteur fournissant au minimum 1 Ampère stable connecté à votre boîtier pour éradiquer tout risque de déconnexion Wi-Fi intempestive.

⚙️ 5. Configuration Initiale et Utilisation de l'IHM Web

📶 a) Configuration initiale du Wi-Fi (AP Fallback)

Si aucune information Wi-Fi n'est présente en mémoire Flash ou si votre box internet est injoignable pendant plus de 20 secondes, l'ESP32 se configure automatiquement en mode Point d'Accès de secours :

Recherchez les réseaux Wi-Fi avec votre smartphone et connectez-vous au Hotspot : RTK_BASE_SETUP.

Une page web (Portail Captif) s'ouvre automatiquement. Si ce n'est pas le cas, rendez-vous sur l'adresse http://192.168.4.1.

Renseignez le SSID (Nom) et le mot de passe de votre réseau Wi-Fi local, puis cliquez sur Sauvegarder. La carte enregistre les informations en mémoire Flash NVS sécurisée et redémarre pour s'y connecter normalement.

📡 b) Passage en mode "Base Station"

Par défaut, le Quectel démarre en mode récepteur mobile (Rover). Pour générer des corrections RTCM3 :

Sur l'IHM Web locale, accédez à la section de configuration du Quectel.

Cliquez sur le bouton "Passer en mode Base Station". L'ESP32 transmet la commande de configuration binaire correspondante au module.

🔄 REDÉMARRAGE ÉLECTRIQUE OBLIGATOIRE : Débranchez puis rebranchez physiquement l'alimentation USB de votre boîtier après cette opération (ou cliquez sur le bouton Reboot). Le Quectel doit redémarrer à froid pour appliquer la modification de registre et commencer à émettre ses trames RTCM3 binaires.

📐 c) Calibrage de Position et Gestion du "Survey-In"

Pour émettre des corrections exploitables par un rover, votre base doit connaître ses coordonnées géographiques au millimètre près :

Le Survey-In automatique : Au premier démarrage en mode Base, la station lance un cycle d'observations géométriques. Par sécurité de précision, ce cycle est configuré d'usine sur 86 400 secondes (24 heures complètes).

Sauvegarde automatique (NVS) : Une fois ce délai écoulé, les coordonnées XYZ tridimensionnelles (ECEF) stables calculées sont figées dans la mémoire Flash (Preferences sous le namespace rtk_base).

Démarrage instantané au boot : Lors des prochains démarrages ou après une coupure d'électricité, le système détecte la position mémorisée, court-circuite le calibrage de 24h, injecte immédiatement les coordonnées géoréférencées dans le Quectel et passe la LED au Vert Fixe en moins de 5 secondes.

🗑️ Déménagement / Bouton Clear : Si vous déplacez physiquement l'antenne sur votre toit, cliquez simplement sur le bouton "Effacer la position en Flash" depuis l'IHM Web. L'ESP32 effacera ses variables NVS et relancera proprement un nouveau cycle de calcul de 24 heures.

💰 d) Configuration du minage Onocoy

Le firmware intègre un connecteur client NTRIP de niveau industriel. La transmission réseau est gérée de manière asynchrone hors du traitement graphique et s'appuie sur la fonction availableForWrite(). Si votre connexion Wi-Fi lag, l'ESP32 rejette le paquet réseau Onocoy obsolète pour donner la priorité absolue à la diffusion locale de votre rover de jardin.

Pour diffuser et accumuler des jetons :

Déclarez votre station sur la console Onocoy Console en y saisissant vos coordonnées issues du Survey-In pour générer vos identifiants de flux uniques.

Accédez à la zone "Configuration Onocoy" de votre IHM Web locale de l'ESP32 et saisissez :

Host : servers.onocoy.com

Port : 2101

Mountpoint (Username) : (Le code d'antenne unique généré par la console Onocoy)

Password : (Le mot de passe de flux associé à cette antenne)

Cliquez sur Enregistrer. La base se connecte, gère la poignée de main et affiche en direct le message d'état : [Handshake Onocoy REUSSI. Minage actif !] dans le terminal de logs, avec le décompte en temps réel des paquets de corrections transmis.
