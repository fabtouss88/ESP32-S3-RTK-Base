#include <WiFi.h>
#include <WebServer.h>
#include <vector>
#include <algorithm>
#include <Adafruit_NeoPixel.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <Preferences.h>

// Version du micrologiciel
#define FIRMWARE_VERSION "1.3.25"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <sys/poll.h>

// Classe WiFiClient personnalisée pour surcharger availableForWrite() et éviter les blocages sur ESP32
class CustomWiFiClient : public WiFiClient {
public:
  using WiFiClient::WiFiClient; // Utilisation des constructeurs de la classe parente
  
  // Cette fonction permet de savoir si le tampon d'écriture TCP est plein.
  // Elle évite d'appeler write() de manière bloquante si le réseau est saturé.
  int availableForWrite() {
    int socketFd = fd();
    if (socketFd < 0) return 0; // Pas de socket valide, donc pas d'espace disponible
    
    // Tentative de récupération de la taille du tampon d'envoi TCP via l'option getsockopt de lwIP
    int val = 0;
    socklen_t len = sizeof(val);
    if (getsockopt(socketFd, IPPROTO_TCP, TCP_SND_BUF, &val, &len) == 0 && val > 0) {
      return val;
    }
    
    // Si getsockopt échoue, on utilise poll() de manière non bloquante
    // C'est beaucoup plus sûr sur ESP32 que select() car cela ne risque pas de corrompre la pile
    struct pollfd pfd;
    pfd.fd = socketFd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int ret = poll(&pfd, 1, 0); // Attente de 0 milliseconde (non bloquant)
    if (ret > 0 && (pfd.revents & POLLOUT)) {
      return 1436; // Si le socket est prêt à écrire, on estime l'espace libre à au moins 1 segment MSS (1436 octets)
    }
    
    return 0; // Le socket est saturé
  }
};

// Variables de configuration pour la connexion Onocoy (gérées dynamiquement)
String onocoy_host = "servers.onocoy.com";
const uint16_t onocoy_port = 2101;
String onocoy_mountpoint = "VOTRE_MOUNTPOINT";
String onocoy_password = "VOTRE_PASSWORD";
IPAddress onocoyIP; // Stocke l'IP résolue du serveur Onocoy
volatile bool onocoyIpResolved = false; // Indique si la résolution DNS de l'hôte Onocoy a réussi

// Broches de communication pour le module GNSS Quectel LC29HEA
#define RX_PIN 16
#define TX_PIN 17
#define GNSS_BAUD 460800

// Configuration de la LED RGB intégrée (sur ESP32-S3 WROOM-1)
#define LED_PIN 48
#define NUMPIXELS 1
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Port réseau par défaut pour le caster NTRIP local et la distribution TCP
#define CASTER_PORT 2101

WebServer server(80); // Serveur Web HTTP sur le port 80
WiFiServer caster(CASTER_PORT); // Serveur TCP pour le caster NTRIP local

// Structure représentant un client NTRIP ou TCP local connecté au caster
struct CasterClient {
  WiFiClient client;
  unsigned long connectTime; // Date de connexion (en millisecondes)
};
std::vector<CasterClient> clients; // Liste dynamique des clients locaux connectés

volatile bool isInternetOk = false; // Indique si l'accès internet fonctionne (test google.com)
volatile bool isAPMode = false; // Indique si la carte fonctionne en mode Point d'Accès autonome de secours
unsigned long lastLogTime = 0;
unsigned long lastClientFlash = 0; // Utilisé pour faire flasher la LED en jaune lors des transferts de paquets

// Verrou (Mutex) FreeRTOS pour sécuriser l'accès concurrent aux données entre les deux cœurs
SemaphoreHandle_t xGNSSMutex = NULL;

#define LOCK_GNSS()    if (xGNSSMutex) { xSemaphoreTake(xGNSSMutex, portMAX_DELAY); }
#define UNLOCK_GNSS()  if (xGNSSMutex) { xSemaphoreGive(xGNSSMutex); }

// Prototype de la tâche de traitement de flux GNSS temps-réel sur le cœur 0
void gnssDataPumpTask(void * pvParameters);

// Variables d'état et de données NMEA
String currentNMEA = ""; // Buffer d'assemblage de la phrase NMEA en cours de réception
int visibleSatellites = 0; // Nombre de satellites visibles
String currentLat = "N/A"; // Latitude formatée
String currentLon = "N/A"; // Longitude formatée
String currentAlt = "N/A"; // Altitude formatée
float latDec = 0.0; // Latitude en degrés décimaux
float lonDec = 0.0; // Longitude en degrés décimaux
String svinStatus = "Initialisation..."; // Statut du calibrage (Survey-In) ou du mode fixe
int svinTimeLeft = 1440 * 60; // Temps restant en secondes pour le calibrage de 24h
String svinMeanX = "0.0000"; // Coordonnée X moyenne calculée (ECEF)
String svinMeanY = "0.0000"; // Coordonnée Y moyenne calculée (ECEF)
String svinMeanZ = "0.0000"; // Coordonnée Z moyenne calculée (ECEF)
String svinMeanAcc = "0.0000"; // Précision de position moyenne estimée

Preferences preferences; // Instance d'accès à la mémoire flash non-volatile (NVS) de l'ESP32
bool hasSavedPos = false; // Indique si une position fixe de base a été préalablement sauvegardée en flash

// Télémétrie et journalisation pour Onocoy
bool onocoyConnected = false; // Indique si la liaison vers Onocoy est active
uint32_t onocoyPacketsSent = 0; // Nombre total de paquets transmis à Onocoy
unsigned long onocoyConnectTime = 0; // Date de connexion à Onocoy
std::vector<String> onocoyLogs; // Historique des logs Onocoy pour l'interface utilisateur

// Formate le temps écoulé en heure:minute:seconde pour les logs
String getFormattedTime() {
  unsigned long totalSeconds = millis() / 1000;
  int seconds = totalSeconds % 60;
  int minutes = (totalSeconds / 60) % 60;
  int hours = (totalSeconds / 3600) % 24;
  char timeStr[15];
  sprintf(timeStr, "[%02d:%02d:%02d] ", hours, minutes, seconds);
  return String(timeStr);
}

// Ajoute un message dans la liste des journaux de télémétrie Onocoy (limité à 15 lignes)
void logOnocoy(String message) {
  String formatted = getFormattedTime() + message;
  LOCK_GNSS();
  onocoyLogs.push_back(formatted);
  if (onocoyLogs.size() > 15) {
    onocoyLogs.erase(onocoyLogs.begin());
  }
  UNLOCK_GNSS();
  Serial.println(formatted);
}

// Structure représentant un satellite pour le tracé graphique du Skyplot
struct Satellite {
  String prn; // Numéro d'identification du satellite (ex: G12, E24)
  int elev; // Élévation en degrés (0 à 90)
  int azim; // Azimut en degrés (0 à 359)
  int snr; // Rapport signal/bruit (dB-Hz)
  String constel; // Constellation (GP pour GPS, GA pour Galileo, GL pour GLONASS, BD pour BeiDou)
  unsigned long lastSeen; // Horodatage de la dernière réception valide
};
std::vector<Satellite> skyplotSats; // Base locale des satellites actuellement suivis

// Console NMEA pour le panneau de débogage web
std::vector<String> nmeaConsole;
const int MAX_CONSOLE_LINES = 30;

// Calcule la somme de contrôle XOR (checksum) et envoie la phrase propriétaire sur la liaison série du Quectel
void computeAndSendChecksum(const char* cmd) {
  byte checksum = 0;
  const char* p = cmd;
  while (*p) {
    checksum ^= *p++;
  }
  char hexChecksum[3];
  sprintf(hexChecksum, "%02X", checksum);
  String finalSentence = "$" + String(cmd) + "*" + hexChecksum;
  
  LOCK_GNSS();
  Serial1.print(finalSentence + "\r\n");
  Serial.print("Envoyé au Quectel : ");
  Serial.println(finalSentence);
  
  // Copie de la commande dans le buffer de console web pour inspection visuelle
  nmeaConsole.push_back("==> ENVOI: " + finalSentence);
  if (nmeaConsole.size() > MAX_CONSOLE_LINES) {
    nmeaConsole.erase(nmeaConsole.begin());
  }
  UNLOCK_GNSS();
}

// Tâche exécutée périodiquement sur le cœur 1 pour vérifier l'accès Internet et résoudre l'IP d'Onocoy
void internetCheckTask(void * pvParameters) {
  for(;;) {
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip;
      // Vérification rapide de la connexion en résolvant google.com
      isInternetOk = WiFi.hostByName("google.com", ip);
      
      IPAddress tmpIP;
      // Résolution DNS de l'adresse du serveur Onocoy pour éviter de bloquer la tâche GNSS en cas de panne réseau
      if (WiFi.hostByName(onocoy_host.c_str(), tmpIP)) {
        onocoyIP = tmpIP;
        onocoyIpResolved = true;
      } else {
        onocoyIpResolved = false;
      }
    } else {
      isInternetOk = false;
      onocoyIpResolved = false;
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS); // Répétition toutes les 5 secondes
  }
}

// Envoie les commandes de configuration de démarrage à la puce GNSS Quectel LC29HEA
void initQuectel() {
  Serial.println("Initialisation du Quectel LC29HEA...");
  // Mode Base Station actif
  computeAndSendChecksum("PQTMCFGRCVRMODE,W,2");
  delay(100);
  computeAndSendChecksum("PAIR432,1");
  delay(100);
  computeAndSendChecksum("PAIR434,1");
  delay(100);
  // Activation forcée des phrases NMEA essentielles pour notre afficheur web
  computeAndSendChecksum("PAIR062,0,1"); // GGA (Position globale et satellites actifs) à 1Hz
  delay(100);
  computeAndSendChecksum("PAIR062,3,1"); // GSV (Détail de la position géométrique de chaque satellite) à 1Hz
  delay(100);
  // Demande d'envoi du message de statut du Survey-In
  computeAndSendChecksum("PQTMCFGMSGRATE,W,PQTMSVINSTATUS,1,1");
  delay(100);
  Serial.println("Initialisation du Quectel terminée.");
}

// Convertit les coordonnées d'angle NMEA (DDMM.MMMM) en degrés décimaux
float nmeaToDec(String nmea, String dir) {
  if (nmea.length() < 4) return 0.0;
  float val = nmea.toFloat();
  int deg = val / 100;
  float min = val - (deg * 100);
  float dec = deg + (min / 60.0);
  if (dir == "S" || dir == "W") dec = -dec;
  return dec;
}

// Met à jour ou ajoute un satellite dans notre table locale pour le tracé graphique
void updateSkyplot(String constel, int prnInt, int elev, int azim, int snr) {
  String prn = constel + String(prnInt);
  bool found = false;
  for (auto& s : skyplotSats) {
    if (s.prn == prn) {
      if (elev >= 0) s.elev = elev;
      if (azim >= 0) s.azim = azim;
      s.snr = snr;
      s.constel = constel;
      s.lastSeen = millis();
      found = true;
      break;
    }
  }
  if (!found) {
    skyplotSats.push_back({prn, elev, azim, snr, constel, millis()});
  }
}

// Valide l'intégrité d'une phrase NMEA en calculant son Checksum XOR
bool verifyChecksum(const String& nmea) {
  int starIndex = nmea.indexOf('*');
  if (starIndex < 0 || starIndex + 3 > nmea.length()) return false;
  byte calculated = 0;
  for (int i = 1; i < starIndex; i++) {
    calculated ^= nmea.charAt(i);
  }
  String hexStr = nmea.substring(starIndex + 1, starIndex + 3);
  byte received = strtol(hexStr.c_str(), NULL, 16);
  return calculated == received;
}

// Parseur NMEA pas à pas qui traite les caractères reçus du port série
void processNMEAByte(char c) {
  if (c == '$') {
    currentNMEA = "$";
  } else if (currentNMEA.length() > 0) {
    if (c == '\r' || c == '\n') {
      if (currentNMEA.length() > 3 && verifyChecksum(currentNMEA)) {
        // Enregistrement dans la console web de débogage
        nmeaConsole.push_back(currentNMEA);
        if (nmeaConsole.size() > MAX_CONSOLE_LINES) {
          nmeaConsole.erase(nmeaConsole.begin());
        }
        
        // Découpage des arguments séparés par des virgules
        int commaIndex = 0;
        int startIndex = 0;
        String parts[25];
        for (int i = 0; i < currentNMEA.length(); i++) {
          if (currentNMEA.charAt(i) == ',' || currentNMEA.charAt(i) == '*') {
            parts[commaIndex++] = currentNMEA.substring(startIndex, i);
            startIndex = i + 1;
            if (commaIndex >= 25) break;
          }
        }
        
        // Traitement de la trame GGA (Coordonnées et qualité)
        if (currentNMEA.startsWith("$G") && currentNMEA.indexOf("GGA") > 0) {
          if (commaIndex >= 10) {
            currentLat = parts[2] + " " + parts[3];
            currentLon = parts[4] + " " + parts[5];
            latDec = nmeaToDec(parts[2], parts[3]);
            lonDec = nmeaToDec(parts[4], parts[5]);
            visibleSatellites = parts[7].toInt();
            currentAlt = parts[9] + " m";
          }
        } 
        // Traitement de la trame GSV (Satellites en vue)
        else if (currentNMEA.indexOf("GSV") > 0) {
          String constel = currentNMEA.substring(1, 3);
          for (int i = 4; i <= commaIndex - 4; i += 4) {
            if (parts[i].length() > 0) {
              int prn = parts[i].toInt();
              int elev = parts[i+1].length() > 0 ? parts[i+1].toInt() : -1;
              int azim = parts[i+2].length() > 0 ? parts[i+2].toInt() : -1;
              int snr = parts[i+3].length() > 0 ? parts[i+3].toInt() : 0;
              updateSkyplot(constel, prn, elev, azim, snr);
            }
          }
        } 
        // Traitement de la trame propriétaire d'état du calibrage (Survey-In)
        else if (currentNMEA.startsWith("$PQTMSVINSTATUS")) {
          if (commaIndex >= 12) {
            int valid = parts[3].toInt();
            int obsTime = parts[6].toInt();
            int cfgDur = parts[7].toInt();
            if (cfgDur <= 0) cfgDur = 86400; // Par défaut 24h
            svinTimeLeft = cfgDur - obsTime;
            if (svinTimeLeft < 0) svinTimeLeft = 0;
            
            svinMeanX = parts[8];
            svinMeanY = parts[9];
            svinMeanZ = parts[10];
            svinMeanAcc = parts[11];
            
            if (hasSavedPos) {
              svinStatus = "Terminé (Fixed Base via Flash)";
              svinTimeLeft = 0;
            } else {
              if (valid == 1) {
                svinStatus = "En cours (Survey-In)";
              } else if (valid == 2 || svinTimeLeft == 0) {
                svinStatus = "Terminé (Fixed Base)";
                svinTimeLeft = 0;
              } else {
                svinStatus = "Attente GNSS... (" + String(valid) + ")";
              }
            }
          }
        }
      }
      currentNMEA = "";
    } else {
      if (currentNMEA.length() < 120) {
        currentNMEA += c;
      } else {
        currentNMEA = ""; // Abandon en cas de phrase trop longue pour éviter de déborder
      }
    }
  }
}

#include "index_html.h" // Fichier d'en-tête contenant les gabarits de pages HTML (index_html et setup_html)

// Machine à états de la LED RVB
void updateLED() {
  LOCK_GNSS();
  unsigned long clientFlash = lastClientFlash;
  int timeLeft = svinTimeLeft;
  UNLOCK_GNSS();
  
  if (hasSavedPos) {
    pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Vert fixe : Base opérationnelle en mode fixe
  } else if (millis() < clientFlash + 200) {
    pixels.setPixelColor(0, pixels.Color(255, 255, 0)); // Clignotement jaune court : Transfert de données NTRIP
  } else if (!isInternetOk || WiFi.status() != WL_CONNECTED) {
    pixels.setPixelColor(0, pixels.Color(255, 0, 0)); // Rouge fixe : Déconnecté du Wi-Fi ou d'Internet
  } else if (timeLeft > 0) {
    pixels.setPixelColor(0, pixels.Color(0, 0, 255)); // Bleu fixe : Calibrage initial (Survey-in) actif
  } else {
    pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Vert fixe : Prêt
  }
  pixels.show();
}

void setup() {
  Serial.begin(115200);
  esp_ota_mark_app_valid_cancel_rollback(); // Confirme le bon démarrage du firmware actuel pour valider la mise à jour OTA
  
  // Initialisation de la mémoire non-volatile NVS
  preferences.begin("rtk_base", false);
  
  // Lecture de la configuration dynamique Onocoy depuis Preferences
  onocoy_host = preferences.getString("ono_host", "servers.onocoy.com");
  if (onocoy_host == "") onocoy_host = "servers.onocoy.com";
  onocoy_mountpoint = preferences.getString("ono_mount", "VOTRE_MOUNTPOINT");
  if (onocoy_mountpoint == "") onocoy_mountpoint = "VOTRE_MOUNTPOINT";
  onocoy_password = preferences.getString("ono_pass", "VOTRE_PASSWORD");
  if (onocoy_password == "") onocoy_password = "VOTRE_PASSWORD";

  String lastBootedVersion = preferences.getString("lastVersion", "");
  if (lastBootedVersion != FIRMWARE_VERSION) {
    preferences.putString("lastVersion", FIRMWARE_VERSION);
  }
  
  // Lecture des coordonnées géographiques de référence enregistrées
  hasSavedPos = preferences.getBool("hasSavedPos", false);
  String rx = preferences.getString("ref_x", "");
  String ry = preferences.getString("ref_y", "");
  String rz = preferences.getString("ref_z", "");
  
  // Si l'une des coordonnées manque en mémoire flash, on désactive le mode fixe automatique
  if (rx == "" || ry == "" || rz == "") {
    hasSavedPos = false;
  }
  
  // Mutex d'accès thread-safe
  xGNSSMutex = xSemaphoreCreateMutex();
  Serial1.setRxBufferSize(65536); // Grand buffer matériel de réception GNSS pour éviter de saturer lors des tâches bloquantes
  Serial1.begin(GNSS_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  
  pixels.begin();
  pixels.setBrightness(50);
  pixels.setPixelColor(0, pixels.Color(255, 0, 0)); // Démarrage en rouge par défaut
  pixels.show();
  
  delay(3000);
  initQuectel();
  
  // Si nous avons une position fixe valide enregistrée en flash, nous l'injectons au Quectel
  if (hasSavedPos) {
    Serial.println("Injection des coordonnées fixes de référence (Mode Base Fixée)...");
    String cmd = "PQTMCFGBXFIXEDXYZ,W,1," + rx + "," + ry + "," + rz;
    computeAndSendChecksum(cmd.c_str());
    delay(200);
    computeAndSendChecksum("PAIR513"); // Écriture en mémoire interne du module GNSS
    delay(200);
    computeAndSendChecksum("PQTMSAVEPAR");
    delay(200);
    computeAndSendChecksum("PAIR023"); // Reboot logiciel du Quectel pour appliquer
    
    delay(6000); // Attente du redémarrage matériel
    initQuectel();
    
    LOCK_GNSS();
    svinStatus = "Terminé (Fixed Base via Flash)";
    svinTimeLeft = 0;
    svinMeanX = rx;
    svinMeanY = ry;
    svinMeanZ = rz;
    svinMeanAcc = "0.0000";
    UNLOCK_GNSS();
  }
  
  // Tentative de lecture de la configuration Wi-Fi
  String wifi_ssid = preferences.getString("wifi_ssid", "");
  String wifi_pass = preferences.getString("wifi_pass", "");
  
  if (wifi_ssid == "") {
    Serial.println("Aucune clé WiFi enregistrée. Lancement du point d'accès de configuration...");
    isAPMode = true;
  } else {
    Serial.print("Connexion au point d'accès Wi-Fi : ");
    Serial.println(wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    WiFi.setAutoReconnect(true); // Reconnexion automatique activée
    
    // Tentative de connexion limitée à un délai d'attente de 20 secondes
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 20000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Échec de connexion après 20s. Repli automatique en mode Point d'Accès...");
      isAPMode = true;
    } else {
      Serial.println("Wi-Fi connecté ! IP locale : " + WiFi.localIP().toString());
      WiFi.setSleep(false); // Désactivation de la mise en veille radio Wi-Fi pour éliminer les latences réseau
    }
  }
  
  // Démarrage du Point d'Accès si nous sommes en mode configuration (Fallback AP Mode)
  if (isAPMode) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("RTK_BASE_SETUP");
    Serial.print("Point d'accès configuré avec succès. SSID: RTK_BASE_SETUP. Accéder à l'IP : ");
    Serial.println(WiFi.softAPIP().toString());
  } else {
    Serial.print("Résolution DNS pour le serveur Onocoy...");
    if (WiFi.hostByName(onocoy_host.c_str(), onocoyIP)) {
      onocoyIpResolved = true;
      Serial.println(" Réussie : " + onocoyIP.toString());
    } else {
      onocoyIpResolved = false;
      Serial.println(" Échouée");
    }
  }
  
  // Paramétrage du protocole de mise à jour logiciel sans fil (Arduino OTA)
  ArduinoOTA.setHostname("BaseRTK");
  ArduinoOTA.onStart([]() {
    Serial.println("Mise à jour du micrologiciel (OTA) commencée...");
    pixels.setPixelColor(0, pixels.Color(255, 0, 255)); // Voyant magenta actif pendant le flash
    pixels.show();
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nMise à jour OTA terminée.");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progression OTA : %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.begin();
  
  // ROUTE WEB RACINE "/" : Affiche l'interface de contrôle ou l'assistant de configuration
  server.on("/", HTTP_GET, [](){
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    if (isAPMode) {
      server.send_P(200, "text/html", setup_html); // Page de configuration si pas de WiFi
    } else {
      server.send_P(200, "text/html", index_html); // Dashboard principal de la station
    }
  });
  
  // ROUTE D'API "/status" (HTTP GET) : Renvoie toutes les données et états au format JSON
  server.on("/status", HTTP_GET, [](){
    LOCK_GNSS();
    String json;
    json.reserve(4096);
    json = "{";
    json += "\"status\":\"" + svinStatus + "\",";
    json += "\"version\":\"" + String(FIRMWARE_VERSION) + "\",";
    json += "\"satellites\":" + String(visibleSatellites) + ",";
    json += "\"lat\":\"" + currentLat + "\",";
    json += "\"lon\":\"" + currentLon + "\",";
    json += "\"alt\":\"" + currentAlt + "\",";
    json += "\"latDec\":" + String(latDec, 6) + ",";
    json += "\"lonDec\":" + String(lonDec, 6) + ",";
    json += "\"timeleft\":" + String(svinTimeLeft) + ",";
    json += "\"internet\":" + String(isInternetOk ? "true" : "false") + ",";
    json += "\"uptime\":" + String(millis() / 1000) + ",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"ip\":\"" + (isAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
    json += "\"ssid\":\"" + (isAPMode ? String("RTK_BASE_SETUP (AP Mode)") : WiFi.SSID()) + "\",";
    json += "\"wifi_ssid\":\"" + preferences.getString("wifi_ssid", "") + "\",";
    json += "\"ref_x\":\"" + preferences.getString("ref_x", "") + "\",";
    json += "\"ref_y\":\"" + preferences.getString("ref_y", "") + "\",";
    json += "\"ref_z\":\"" + preferences.getString("ref_z", "") + "\",";
    json += "\"ram\":" + String(ESP.getFreeHeap() / 1024) + ",";
    json += "\"temp\":" + String(temperatureRead(), 1) + ",";
    json += "\"meanX\":\"" + svinMeanX + "\",";
    json += "\"meanY\":\"" + svinMeanY + "\",";
    json += "\"meanZ\":\"" + svinMeanZ + "\",";
    json += "\"meanAcc\":\"" + svinMeanAcc + "\",";
    json += "\"ono_host\":\"" + onocoy_host + "\",";
    json += "\"ono_mount\":\"" + onocoy_mountpoint + "\",";
    json += "\"ono_pass\":\"" + onocoy_password + "\",";
    
    // Récupération de l'historique des logs Onocoy
    String onocoyConsole = "";
    for (size_t i = 0; i < onocoyLogs.size(); i++) {
      String tmp = onocoyLogs[i];
      tmp.replace("\"", "\\\"");
      onocoyConsole += tmp;
      if (i < onocoyLogs.size() - 1) onocoyConsole += "\\n";
    }
    json += "\"onocoy_status\":\"" + String(onocoyConnected ? "Connecté" : "Déconnecté") + "\",";
    json += "\"onocoy_packets\":" + String(onocoyPacketsSent) + ",";
    json += "\"onocoy_uptime\":" + String(onocoyConnected ? (millis() - onocoyConnectTime) / 1000 : 0) + ",";
    json += "\"onocoy_console\":\"" + onocoyConsole + "\",";
    
    json += "\"clientsCount\":" + String(clients.size()) + ",";
    json += "\"clients\":[";
    unsigned long now = millis();
    for (size_t i = 0; i < clients.size(); i++) {
      json += "{";
      json += "\"ip\":\"" + clients[i].client.remoteIP().toString() + "\",";
      json += "\"duration\":" + String((now - clients[i].connectTime) / 1000);
      json += "}";
      if (i < clients.size() - 1) json += ",";
    }
    json += "],";
    
    // Nettoyage des vieux satellites du Skyplot (absents depuis plus de 15s) et intégration au JSON
    json += "\"skyplot\":[";
    skyplotSats.erase(std::remove_if(skyplotSats.begin(), skyplotSats.end(), [](const Satellite& s) {
        return millis() - s.lastSeen > 15000;
    }), skyplotSats.end());
    
    bool firstSat = true;
    for(size_t i = 0; i < skyplotSats.size(); i++) {
      if (skyplotSats[i].elev < 0 || skyplotSats[i].azim < 0) continue;
      if (!firstSat) json += ",";
      firstSat = false;
      json += "{";
      json += "\"prn\":\"" + skyplotSats[i].prn + "\",";
      json += "\"elev\":" + String(skyplotSats[i].elev) + ",";
      json += "\"azim\":" + String(skyplotSats[i].azim) + ",";
      json += "\"snr\":" + String(skyplotSats[i].snr) + ",";
      json += "\"constel\":\"" + skyplotSats[i].constel + "\"";
      json += "}";
    }
    json += "]";
    json += "}";
    UNLOCK_GNSS();
    server.send(200, "application/json", json);
  });
  
  // ROUTE WEB "/nmea" (HTTP GET) : Renvoie le journal NMEA brut pour l'affichage de la console web
  server.on("/nmea", HTTP_GET, [](){
    LOCK_GNSS();
    String out = "";
    for(auto& s : nmeaConsole) out += s + "\n";
    UNLOCK_GNSS();
    server.send(200, "text/plain", out);
  });
  
  // ROUTE WEB "/reboot" (HTTP POST) : Redémarre l'ESP32 immédiatement
  server.on("/reboot", HTTP_POST, [](){
    Serial.println("Demande utilisateur : Redémarrage en cours...");
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });
  
  // ROUTE WEB "/update" (HTTP GET) : Affiche la page d'envoi du firmware (.bin) via HTTP
  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    String html = "<html><body style='background:#121212;color:white;font-family:sans-serif;text-align:center;padding:50px;'>";
    html += "<h2 style='color:#00ADB5;'>Mise à jour Web (OTA HTTP)</h2>";
    html += "<p style='color:#888;'>Version actuelle : v" + String(FIRMWARE_VERSION) + "</p>";
    html += "<p>Sélectionnez le fichier micrologiciel (ex: BASERTK.ino.bin)</p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='update' accept='.bin' style='margin:20px; padding:10px; background:#2b2b2b; color:white;'><br>";
    html += "<input type='submit' value='Flasher la carte' style='padding:15px 30px; background:#00ADB5; color:black; font-weight:bold; border:none; border-radius:5px; cursor:pointer;'>";
    html += "</form></body></html>";
    server.send(200, "text/html", html);
  });

  // ROUTE WEB "/update" (HTTP POST) : Gère le flux de flash du fichier binaire et redémarre
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    bool hasError = Update.hasError();
    String msg = hasError ? "ÉCHEC du flashage ! Vérifiez le fichier." : "SUCCÈS ! Redémarrage de l'antenne...";
    String html = "<html><body style='background:#121212;color:white;font-family:sans-serif;text-align:center;padding:50px;'>";
    html += "<h2 style='color:" + String(hasError ? "#e74c3c" : "#2ecc71") + ";'>" + msg + "</h2>";
    html += "<p><a href='/' style='color:#00ADB5;'>Retourner à la page d'accueil</a></p>";
    if(!hasError) html += "<script>setTimeout(function(){window.location.href='/';}, 10000);</script>";
    html += "</body></html>";
    server.send(200, "text/html", html);
    if (!hasError) {
      delay(1000);
      ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Début du flashage par Web OTA : %s\n", upload.filename.c_str());
      pixels.setPixelColor(0, pixels.Color(0, 255, 255)); // Cyan pendant le flash
      pixels.show();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Web OTA réussi ! Taille : %u octets\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  // ROUTE WEB "/start_svin" (HTTP POST) : Démarre manuellement un calibrage GNSS de 24h
  server.on("/start_svin", HTTP_POST, [](){
    Serial.println("Action : Lancement du Survey-In (24 heures)...");
    computeAndSendChecksum("PQTMCFGMSGRATE,W,PQTMSVINSTATUS,1,1");
    delay(200);
    computeAndSendChecksum("PAIR062,0,1");
    delay(200);
    computeAndSendChecksum("PAIR062,3,1");
    delay(200);
    // Commande de démarrage du calibrage (précision cible de 3.0 mètres pour accepter les mesures de départ)
    computeAndSendChecksum("PQTMCFGSVIN,W,1,86400,3.0,0,0,0");
    delay(200);
    computeAndSendChecksum("PAIR513");
    delay(200);
    computeAndSendChecksum("PAIR023");
    
    preferences.putBool("hasSavedPos", false); // On efface le drapeau de position fixe en NVS
    
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });
  
  // ROUTE WEB "/test_quectel" (HTTP POST) : Interroge le Quectel pour valider la communication
  server.on("/test_quectel", HTTP_POST, [](){
    Serial.println("Action : Test de communication série avec le Quectel...");
    computeAndSendChecksum("PQTMVERNO");
    delay(200);
    computeAndSendChecksum("PQTMCFGSVIN,R");
    server.send(200, "text/plain", "OK");
  });
  
  // ROUTE WEB "/init_base" (HTTP POST) : Configure par défaut le Quectel en Base GNSS et démarre
  server.on("/init_base", HTTP_POST, [](){
    Serial.println("Action : Forçage de la configuration d'usine Base Station...");
    computeAndSendChecksum("PAIR514"); // Restauration des configurations par défaut du récepteur GNSS
    delay(200);
    computeAndSendChecksum("PQTMCFGRCVRMODE,W,2"); // Mode station de référence
    delay(200);
    computeAndSendChecksum("PAIR062,0,1");
    delay(200);
    computeAndSendChecksum("PAIR062,3,1");
    delay(200);
    computeAndSendChecksum("PQTMCFGMSGRATE,W,PQTMSVINSTATUS,1,1");
    delay(200);
    computeAndSendChecksum("PQTMCFGSVIN,W,1,86400,3.0,0,0,0");
    delay(200);
    computeAndSendChecksum("PAIR513");
    delay(200);
    computeAndSendChecksum("PAIR023");
    
    preferences.putBool("hasSavedPos", false);
    
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });

  // ROUTE WEB "/fix_base" (HTTP POST) : Fige de manière définitive le Quectel avec sa position courante
  server.on("/fix_base", HTTP_POST, [](){
    LOCK_GNSS();
    String x = svinMeanX;
    String y = svinMeanY;
    String z = svinMeanZ;
    UNLOCK_GNSS();
    
    Serial.println("Action : Figer la base sur la position moyenne courante...");
    Serial.printf("X: %s, Y: %s, Z: %s\n", x.c_str(), y.c_str(), z.c_str());
    
    if (x != "0.0000" && x != "") {
      // Configuration en mode fixed permanent
      String cmd = "PQTMCFGSVIN,W,2,0,0," + x + "," + y + "," + z;
      computeAndSendChecksum(cmd.c_str());
      delay(200);
      computeAndSendChecksum("PAIR513");
      delay(200);
      computeAndSendChecksum("PQTMSAVEPAR");
      delay(200);
      computeAndSendChecksum("PAIR023");
      
      preferences.putString("ref_x", x);
      preferences.putString("ref_y", y);
      preferences.putString("ref_z", z);
      preferences.putBool("hasSavedPos", true);
      
      server.send(200, "text/plain", "OK");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Coordonnées courantes invalides (valeurs nulles)");
    }
  });

  // ROUTE WEB "/clear_position" (HTTP POST) : Supprime la position fixe et repasse en Survey-In
  server.on("/clear_position", HTTP_POST, [](){
    Serial.println("Action : Suppression de la position de référence en flash...");
    preferences.putBool("hasSavedPos", false);
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });
  
  // ROUTE WEB "/save_config" (HTTP POST) : Enregistre la configuration Onocoy saisie sur l'IHM
  server.on("/save_config", HTTP_POST, [](){
    if (server.hasArg("ono_host") && server.hasArg("ono_mount") && server.hasArg("ono_pass")) {
      String host = server.arg("ono_host");
      String mount = server.arg("ono_mount");
      String pass = server.arg("ono_pass");
      
      host.trim();
      mount.trim();
      pass.trim();
      
      preferences.putString("ono_host", host);
      preferences.putString("ono_mount", mount);
      preferences.putString("ono_pass", pass);
      
      Serial.println("Mise à jour Onocoy enregistrée avec succès.");
      
      server.send(200, "text/plain", "OK");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Champs de saisie manquants");
    }
  });

  // ROUTE WEB "/save_system" (HTTP POST) : Enregistre toute la configuration (Wi-Fi, Onocoy, Coordonnées) en mode AP
  server.on("/save_system", HTTP_POST, [](){
    if (server.hasArg("wifi_ssid") && server.hasArg("wifi_pass") &&
        server.hasArg("ono_host") && server.hasArg("ono_mount") && server.hasArg("ono_pass")) {
      
      String ssid_val = server.arg("wifi_ssid");
      String pass_val = server.arg("wifi_pass");
      String host_val = server.arg("ono_host");
      String mount_val = server.arg("ono_mount");
      String ono_pass_val = server.arg("ono_pass");
      
      ssid_val.trim();
      pass_val.trim();
      host_val.trim();
      mount_val.trim();
      ono_pass_val.trim();
      
      preferences.putString("wifi_ssid", ssid_val);
      preferences.putString("wifi_pass", pass_val);
      preferences.putString("ono_host", host_val);
      preferences.putString("ono_mount", mount_val);
      preferences.putString("ono_pass", ono_pass_val);
      
      if (server.hasArg("ref_x") && server.hasArg("ref_y") && server.hasArg("ref_z")) {
        String rx_val = server.arg("ref_x");
        String ry_val = server.arg("ref_y");
        String rz_val = server.arg("ref_z");
        
        rx_val.trim();
        ry_val.trim();
        rz_val.trim();
        
        if (rx_val != "" && ry_val != "" && rz_val != "") {
          preferences.putString("ref_x", rx_val);
          preferences.putString("ref_y", ry_val);
          preferences.putString("ref_z", rz_val);
          preferences.putBool("hasSavedPos", true);
        } else {
          preferences.putString("ref_x", "");
          preferences.putString("ref_y", "");
          preferences.putString("ref_z", "");
          preferences.putBool("hasSavedPos", false);
        }
      } else {
        preferences.putString("ref_x", "");
        preferences.putString("ref_y", "");
        preferences.putString("ref_z", "");
        preferences.putBool("hasSavedPos", false);
      }
      
      String response = "<html><body style='background:#121212;color:white;font-family:sans-serif;text-align:center;padding:50px;'>";
      response += "<h2 style='color:#2ecc71;'>Configuration système sauvegardée !</h2>";
      response += "<p>Redémarrage de l'antenne... Elle va se connecter au Wi-Fi : <b>" + ssid_val + "</b></p>";
      response += "</body></html>";
      server.send(200, "text/html", response);
      
      delay(2000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Champs requis manquants");
    }
  });

  // ROUTE WEB "/save_wifi" (HTTP POST) : Met à jour uniquement le SSID et mot de passe Wi-Fi
  server.on("/save_wifi", HTTP_POST, [](){
    if (server.hasArg("wifi_ssid") && server.hasArg("wifi_pass")) {
      String ssid_val = server.arg("wifi_ssid");
      String pass_val = server.arg("wifi_pass");
      
      ssid_val.trim();
      pass_val.trim();
      
      preferences.putString("wifi_ssid", ssid_val);
      preferences.putString("wifi_pass", pass_val);
      
      Serial.println("Sauvegarde du SSID Wi-Fi et redémarrage de la carte...");
      
      server.send(200, "text/plain", "OK");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Champs requis manquants");
    }
  });

  // ROUTE WEB "/save_coords" (HTTP POST) : Définit manuellement des coordonnées XYZ ECEF de référence
  server.on("/save_coords", HTTP_POST, [](){
    if (server.hasArg("ref_x") && server.hasArg("ref_y") && server.hasArg("ref_z")) {
      String x_val = server.arg("ref_x");
      String y_val = server.arg("ref_y");
      String z_val = server.arg("ref_z");
      
      x_val.trim();
      y_val.trim();
      z_val.trim();
      
      if (x_val != "" && y_val != "" && z_val != "") {
        preferences.putString("ref_x", x_val);
        preferences.putString("ref_y", y_val);
        preferences.putString("ref_z", z_val);
        preferences.putBool("hasSavedPos", true); // On force la base en mode fixe à sa position
        
        Serial.println("Configuration manuelle des coordonnées sauvegardée.");
        
        server.send(200, "text/plain", "OK");
        delay(1000);
        ESP.restart();
      } else {
        server.send(400, "text/plain", "Valeurs de coordonnées invalides");
      }
    } else {
      server.send(400, "text/plain", "Champs requis manquants");
    }
  });
  
  server.begin();
  Serial.println("Serveur Web HTTP démarré sur le port 80.");
  
  // Tâche de surveillance réseau basse priorité
  xTaskCreate(internetCheckTask, "InternetCheck", 2048, NULL, 1, NULL);

  caster.begin();
  Serial.println("Caster TCP NTRIP local prêt sur le port " + String(CASTER_PORT));

  // Lancement de la tâche GNSS critique sur le cœur 0 (PRO_CPU) avec priorité 5
  xTaskCreatePinnedToCore(
    gnssDataPumpTask,
    "gnssDataPumpTask",
    8192,
    NULL,
    5,
    NULL,
    0
  );
}

// Tâche FreeRTOS temps-réel sur le cœur 0 (gestionnaire des paquets GNSS et réseau)
void gnssDataPumpTask(void * pvParameters) {
  uint8_t buffer[1024];
  static CustomWiFiClient onocoyClient;
  onocoyClient.setTimeout(50); // Timeout très court de 50ms pour ne jamais bloquer la boucle lors de l'envoi réseau
  static unsigned long lastOnocoyAttempt = 0;

  for (;;) {
    // Machine à états asynchrone non-bloquante pour la diffusion NTRIP vers Onocoy
    if (WiFi.status() == WL_CONNECTED) {
      if (!onocoyConnected && !onocoyClient.connected()) {
        // Tentative de reconnexion toutes les 10 secondes
        if (millis() - lastOnocoyAttempt > 10000) {
          lastOnocoyAttempt = millis();
          if (!onocoyIpResolved || onocoyIP == IPAddress(0,0,0,0)) {
            logOnocoy("Connexion impossible : résolution DNS Onocoy échouée.");
          } else {
            logOnocoy("Connexion au serveur Onocoy (" + onocoyIP.toString() + ")...");
            if (onocoyClient.connect(onocoyIP, onocoy_port)) {
              // Envoi de l'en-tête protocolaire SOURCE NTRIP
              onocoyClient.print("SOURCE ");
              onocoyClient.print(onocoy_password);
              onocoyClient.print(" /");
              onocoyClient.print(onocoy_mountpoint);
              onocoyClient.print("\r\n");
              onocoyClient.print("Source-Agent: ESP32-S3-RTK/" FIRMWARE_VERSION "\r\n");
              onocoyClient.print("\r\n");
              onocoyClient.flush();
              
              // Attente de validation de la connexion (max 1500ms)
              unsigned long startWait = millis();
              while (!onocoyClient.available() && millis() - startWait < 1500) {
                vTaskDelay(10 / portTICK_PERIOD_MS);
              }
              
              String response = "";
              if (onocoyClient.available()) {
                response = onocoyClient.readStringUntil('\n');
              }
              
              if (response.indexOf("200 OK") >= 0) {
                LOCK_GNSS();
                onocoyConnected = true;
                onocoyConnectTime = millis();
                UNLOCK_GNSS();
                logOnocoy("Authentification réussie ! Diffusion active.");
              } else {
                logOnocoy("Refus d'authentification Onocoy. Réponse : " + response);
                onocoyClient.stop();
              }
            } else {
              logOnocoy("Échec de la connexion TCP vers Onocoy.");
              onocoyClient.stop();
            }
          }
        }
      } else if (onocoyConnected && !onocoyClient.connected()) {
        LOCK_GNSS();
        onocoyConnected = false;
        UNLOCK_GNSS();
        logOnocoy("Serveur Onocoy déconnecté.");
      }
    } else {
      if (onocoyConnected || onocoyClient.connected()) {
        LOCK_GNSS();
        onocoyConnected = false;
        UNLOCK_GNSS();
        onocoyClient.stop();
        logOnocoy("Perte Wi-Fi : Connexion Onocoy interrompue.");
      }
    }

    // 1. Détection et poignée de main NTRIP non-bloquante pour les clients locaux (caster TCP)
    if (caster.hasClient()) {
      WiFiClient newClient = caster.available();
      if (newClient) {
        newClient.setTimeout(10); // Timeout très court de 10ms pour éviter de ralentir la pompe à données
        Serial.printf("\nNouveau client Caster connecté : %s\n", newClient.remoteIP().toString().c_str());
        
        // Attente courte des en-têtes NTRIP clients (max 50ms)
        int delayCount = 0;
        while (!newClient.available() && delayCount < 10) {
          vTaskDelay(5 / portTICK_PERIOD_MS);
          delayCount++;
        }
        
        if (newClient.available()) {
          String request = newClient.readStringUntil('\n');
          if (request.startsWith("GET /") || request.startsWith("GET ")) {
            newClient.print("ICY 200 OK\r\n\r\n"); // Réponse NTRIP standard
            newClient.flush();
            Serial.println("Réponse NTRIP envoyée : ICY 200 OK");
          }
        }
        
        LOCK_GNSS();
        clients.push_back({newClient, millis()});
        lastClientFlash = millis(); // Flash de la LED en jaune
        UNLOCK_GNSS();
      }
    }
    
    // Nettoyage de la table des clients locaux déconnectés
    LOCK_GNSS();
    for (int i = clients.size() - 1; i >= 0; i--) {
      if (!clients[i].client.connected()) {
        Serial.println("\nClient local déconnecté.");
        clients.erase(clients.begin() + i);
      }
    }
    UNLOCK_GNSS();

    // 2. Lecture agressive et vidage du tampon série du module Quectel
    size_t bytesRead = 0;
    while (Serial1.available() && bytesRead < sizeof(buffer)) {
      buffer[bytesRead++] = Serial1.read();
    }

    // 3. Diffusion instantanée des données aux rovers locaux et au parseur de télémétrie
    if (bytesRead > 0) {
      // Duplication de la liste des clients locaux connectés (sous mutex court)
      std::vector<WiFiClient> activeClients;
      LOCK_GNSS();
      for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].client.connected()) {
          activeClients.push_back(clients[i].client);
        }
      }
      UNLOCK_GNSS();

      // Envoi du flux brut aux rovers locaux (hors mutex)
      for (size_t i = 0; i < activeClients.size(); i++) {
        if (activeClients[i].connected()) {
          activeClients[i].write(buffer, bytesRead);
        }
      }

      // Injection et traitement dans le parseur NMEA (sous mutex)
      LOCK_GNSS();
      for (size_t i = 0; i < bytesRead; i++) {
        char c = (char)buffer[i];
        
        // Filtre d'optimisation CPU : on rejette les octets binaires RTCM pour ne traiter que l'ASCII NMEA
        bool isPrintable = (c >= 32 && c <= 126) || c == '\r' || c == '\n';
        if (!isPrintable) {
          continue;
        }
        if (currentNMEA.length() == 0 && c != '$') {
          continue; // On ignore les caractères isolés hors phrase NMEA
        }
        processNMEAByte(c);
      }
      bool sendToOnocoy = onocoyConnected;
      UNLOCK_GNSS();

      // Envoi du flux brut vers Onocoy (hors mutex et 100% non-bloquant)
      if (sendToOnocoy && onocoyClient.connected()) {
        // Drop immédiat du paquet si le buffer TCP de l'ESP32 est saturé
        if (onocoyClient.availableForWrite() >= bytesRead) {
          onocoyClient.write(buffer, bytesRead);
          LOCK_GNSS();
          onocoyPacketsSent++;
          UNLOCK_GNSS();
        }
      }
    }

    vTaskDelay(2 / portTICK_PERIOD_MS); // Petite attente de 2ms pour laisser la main au planificateur FreeRTOS
  }
}

// Boucle principale sur le cœur 1
void loop() {
  server.handleClient(); // Traitement des requêtes HTTP reçues
  ArduinoOTA.handle(); // Écoute et gestion des demandes de flash sans fil
  updateLED(); // Rafraîchissement de l'affichage lumineux de la LED RVB
  
  // Envoi régulier de reconfiguration pour valider la sortie NMEA sur le module GNSS
  static unsigned long lastQuectelConfig = 0;
  if (millis() - lastQuectelConfig > 900000) { // Exécuté toutes les 15 minutes
    lastQuectelConfig = millis();
    computeAndSendChecksum("PAIR062,0,1"); // Activation de GGA (1Hz)
    delay(50);
    computeAndSendChecksum("PAIR062,3,1"); // Activation de GSV (1Hz)
    delay(50);
  }
  
  delay(1); // Maintien de l'ordonnancement système
}
