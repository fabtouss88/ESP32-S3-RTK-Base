#include <WiFi.h>
#include <WebServer.h>
#include <vector>
#include <algorithm>
#include <Adafruit_NeoPixel.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <Preferences.h>

#define FIRMWARE_VERSION "1.3.25"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <sys/poll.h>

// Client WiFi personnalisé pour surcharger availableForWrite() sur ESP32
class CustomWiFiClient : public WiFiClient {
public:
  using WiFiClient::WiFiClient; // Hériter des constructeurs
  
  int availableForWrite() {
    int socketFd = fd();
    if (socketFd < 0) return 0;
    
    // Tenter de lire l'espace libre via getsockopt (lwIP TCP_SND_BUF)
    int val = 0;
    socklen_t len = sizeof(val);
    if (getsockopt(socketFd, IPPROTO_TCP, TCP_SND_BUF, &val, &len) == 0 && val > 0) {
      return val;
    }
    
    // Repli de secours : tester l'état d'écriture avec poll (VFS-safe, évite FD_SETSIZE de select)
    struct pollfd pfd;
    pfd.fd = socketFd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int ret = poll(&pfd, 1, 0);
    if (ret > 0 && (pfd.revents & POLLOUT)) {
      return 1436; // Estimé à 1 segment MSS minimum
    }
    
    return 0;
  }
};

// Configuration Dynamique Onocoy
String onocoy_host = "servers.onocoy.com";
const uint16_t onocoy_port = 2101;
String onocoy_mountpoint = "VOTRE_MOUNTPOINT";
String onocoy_password = "VOTRE_PASSWORD";
IPAddress onocoyIP;
volatile bool onocoyIpResolved = false;

// Configuration Quectel LC29HEA
#define RX_PIN 16
#define TX_PIN 17
#define GNSS_BAUD 460800

// Configuration LED RGB (S3 WROOM)
#define LED_PIN 48
#define NUMPIXELS 1
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Port NTRIP / TCP
#define CASTER_PORT 2101

WebServer server(80);
WiFiServer caster(CASTER_PORT);

struct CasterClient {
  WiFiClient client;
  unsigned long connectTime;
};
std::vector<CasterClient> clients;

volatile bool isInternetOk = false;
volatile bool isAPMode = false;
unsigned long lastLogTime = 0;
unsigned long lastClientFlash = 0;

// Mutex FreeRTOS pour l'accès concurrent thread-safe
SemaphoreHandle_t xGNSSMutex = NULL;

#define LOCK_GNSS()    if (xGNSSMutex) { xSemaphoreTake(xGNSSMutex, portMAX_DELAY); }
#define UNLOCK_GNSS()  if (xGNSSMutex) { xSemaphoreGive(xGNSSMutex); }

// Prototype de la tâche agressive GNSS sur le cœur 0
void gnssDataPumpTask(void * pvParameters);

// Variables globales NMEA
String currentNMEA = "";
int visibleSatellites = 0;
String currentLat = "N/A";
String currentLon = "N/A";
String currentAlt = "N/A";
float latDec = 0.0;
float lonDec = 0.0;
String svinStatus = "Initialisation...";
int svinTimeLeft = 1440 * 60; // en secondes (24h)
String svinMeanX = "0.0000";
String svinMeanY = "0.0000";
String svinMeanZ = "0.0000";
String svinMeanAcc = "0.0000";

Preferences preferences;
bool hasSavedPos = false;

// Télémétrie et logs Onocoy
bool onocoyConnected = false;
uint32_t onocoyPacketsSent = 0;
unsigned long onocoyConnectTime = 0;
std::vector<String> onocoyLogs;

String getFormattedTime() {
  unsigned long totalSeconds = millis() / 1000;
  int seconds = totalSeconds % 60;
  int minutes = (totalSeconds / 60) % 60;
  int hours = (totalSeconds / 3600) % 24;
  char timeStr[15];
  sprintf(timeStr, "[%02d:%02d:%02d] ", hours, minutes, seconds);
  return String(timeStr);
}

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

// Skyplot
struct Satellite {
  String prn;
  int elev;
  int azim;
  int snr;
  String constel;
  unsigned long lastSeen;
};
std::vector<Satellite> skyplotSats;

// Buffer NMEA pour la console web
std::vector<String> nmeaConsole;
const int MAX_CONSOLE_LINES = 30;

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
  Serial.print("Sent to Quectel: ");
  Serial.println(finalSentence);
  
  // Log dans la console web pour pouvoir deboguer
  nmeaConsole.push_back("==> ENVOI: " + finalSentence);
  if (nmeaConsole.size() > MAX_CONSOLE_LINES) {
    nmeaConsole.erase(nmeaConsole.begin());
  }
  UNLOCK_GNSS();
}

void internetCheckTask(void * pvParameters) {
  for(;;) {
    if (WiFi.status() == WL_CONNECTED) {
      IPAddress ip;
      isInternetOk = WiFi.hostByName("google.com", ip);
      
      IPAddress tmpIP;
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
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void initQuectel() {
  Serial.println("Initialisation du Quectel LC29HEA...");
  // Configurer le mode Base Station
  computeAndSendChecksum("PQTMCFGRCVRMODE,W,2");
  delay(100);
  computeAndSendChecksum("PAIR432,1");
  delay(100);
  computeAndSendChecksum("PAIR434,1");
  delay(100);
  // S'assurer que le module sort les phrases NMEA indispensables pour l'interface web (GGA et GSV)
  computeAndSendChecksum("PAIR062,0,1"); // Active GGA (1Hz)
  delay(100);
  computeAndSendChecksum("PAIR062,3,1"); // Active GSV (1Hz)
  delay(100);
  // Activer l'envoi régulier du statut Survey-In
  computeAndSendChecksum("PQTMCFGMSGRATE,W,PQTMSVINSTATUS,1,1");
  delay(100);
  Serial.println("Initialisation terminée.");
}

float nmeaToDec(String nmea, String dir) {
  if (nmea.length() < 4) return 0.0;
  float val = nmea.toFloat();
  int deg = val / 100;
  float min = val - (deg * 100);
  float dec = deg + (min / 60.0);
  if (dir == "S" || dir == "W") dec = -dec;
  return dec;
}

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

void processNMEAByte(char c) {
  if (c == '$') {
    currentNMEA = "$";
  } else if (currentNMEA.length() > 0) {
    if (c == '\r' || c == '\n') {
      if (currentNMEA.length() > 3 && verifyChecksum(currentNMEA)) {
        // Ajout à la console web
        nmeaConsole.push_back(currentNMEA);
        if (nmeaConsole.size() > MAX_CONSOLE_LINES) {
          nmeaConsole.erase(nmeaConsole.begin());
        }
        
        // Séparation par virgule
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
        
        if (currentNMEA.startsWith("$G") && currentNMEA.indexOf("GGA") > 0) {
          if (commaIndex >= 10) {
            currentLat = parts[2] + " " + parts[3];
            currentLon = parts[4] + " " + parts[5];
            latDec = nmeaToDec(parts[2], parts[3]);
            lonDec = nmeaToDec(parts[4], parts[5]);
            visibleSatellites = parts[7].toInt();
            currentAlt = parts[9] + " m";
          }
        } else if (currentNMEA.indexOf("GSV") > 0) {
          String constel = currentNMEA.substring(1, 3);
          // Les infos des satellites commencent à l'index 4, 4 par 4 (PRN, Elev, Azim, SNR)
          for (int i = 4; i <= commaIndex - 4; i += 4) {
            if (parts[i].length() > 0) {
              int prn = parts[i].toInt();
              int elev = parts[i+1].length() > 0 ? parts[i+1].toInt() : -1;
              int azim = parts[i+2].length() > 0 ? parts[i+2].toInt() : -1;
              int snr = parts[i+3].length() > 0 ? parts[i+3].toInt() : 0;
              updateSkyplot(constel, prn, elev, azim, snr);
            }
          }
        } else if (currentNMEA.startsWith("$PQTMSVINSTATUS")) {
          // Format: $PQTMSVINSTATUS,MsgVer,TOW,Valid,Res0,Res1,Obs,CfgDur,MeanX,MeanY,MeanZ,MeanAcc*CS
          if (commaIndex >= 12) {
            int valid = parts[3].toInt();
            int obsTime = parts[6].toInt();
            int cfgDur = parts[7].toInt();
            if (cfgDur <= 0) cfgDur = 86400; // Fallback
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
        currentNMEA = ""; // Discard oversized sentence
      }
    }
  }
}

#include "index_html.h"


// On gère la machine à état LED
void updateLED() {
  LOCK_GNSS();
  unsigned long clientFlash = lastClientFlash;
  int timeLeft = svinTimeLeft;
  UNLOCK_GNSS();
  
  if (hasSavedPos) {
    pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Vert (Fige)
  } else if (millis() < clientFlash + 200) {
    pixels.setPixelColor(0, pixels.Color(255, 255, 0)); // Jaune (Flash)
  } else if (!isInternetOk || WiFi.status() != WL_CONNECTED) {
    pixels.setPixelColor(0, pixels.Color(255, 0, 0)); // Rouge (Pas de Wifi/Internet)
  } else if (timeLeft > 0) {
    pixels.setPixelColor(0, pixels.Color(0, 0, 255)); // Bleu (Survey-in)
  } else {
    pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Vert (Prêt / Fixed)
  }
  pixels.show();
}

void setup() {
  Serial.begin(115200);
  esp_ota_mark_app_valid_cancel_rollback();
  
  // Initialisation de Preferences
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
  
  hasSavedPos = preferences.getBool("hasSavedPos", false);
  String rx = preferences.getString("ref_x", "");
  String ry = preferences.getString("ref_y", "");
  String rz = preferences.getString("ref_z", "");
  
  if (rx == "" || ry == "" || rz == "") {
    hasSavedPos = false;
  }
  
  xGNSSMutex = xSemaphoreCreateMutex();
  Serial1.setRxBufferSize(65536);
  Serial1.begin(GNSS_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  
  pixels.begin();
  pixels.setBrightness(50);
  pixels.setPixelColor(0, pixels.Color(255, 0, 0));
  pixels.show();
  
  delay(3000);
  initQuectel();
  
  if (hasSavedPos) {
    Serial.println("Injection des coordonnees fixes (Base Geo-referencee)...");
    String cmd = "PQTMCFGBXFIXEDXYZ,W,1," + rx + "," + ry + "," + rz;
    computeAndSendChecksum(cmd.c_str());
    delay(200);
    // Sauvegarder dans la mémoire NVRAM du module via PAIR et PQTMSAVEPAR
    computeAndSendChecksum("PAIR513");
    delay(200);
    computeAndSendChecksum("PQTMSAVEPAR");
    delay(200);
    // Rebooter le module Quectel via PAIR
    computeAndSendChecksum("PAIR023");
    
    // Attendre que le module redémarre et ré-appliquer les configurations
    delay(6000);
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
  
  // 1. Lire la config WiFi depuis Preferences
  String wifi_ssid = preferences.getString("wifi_ssid", "");
  String wifi_pass = preferences.getString("wifi_pass", "");
  
  if (wifi_ssid == "") {
    Serial.println("Pas de SSID WiFi configure en NVS. Passage en mode Point d'Acces...");
    isAPMode = true;
  } else {
    Serial.print("Connexion au WiFi: ");
    Serial.println(wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    WiFi.setAutoReconnect(true);
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 20000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Echec de connexion WiFi apres 20 secondes. Passage en mode Point d'Acces...");
      isAPMode = true;
    } else {
      Serial.println("WiFi connecte. IP: " + WiFi.localIP().toString());
      WiFi.setSleep(false);
    }
  }
  
  if (isAPMode) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("RTK_BASE_SETUP");
    Serial.print("Point d'Acces actif. SSID: RTK_BASE_SETUP. IP: ");
    Serial.println(WiFi.softAPIP().toString());
  } else {
    Serial.print("Resolution DNS unique pour Onocoy...");
    if (WiFi.hostByName(onocoy_host.c_str(), onocoyIP)) {
      onocoyIpResolved = true;
      Serial.println(" Reussie: " + onocoyIP.toString());
    } else {
      onocoyIpResolved = false;
      Serial.println(" Echee");
    }
  }
  
  // Configuration OTA
  ArduinoOTA.setHostname("BaseRTK");
  // ArduinoOTA.setPassword("rtk1234"); // Décommenter si tu veux un mot de passe pour flasher
  ArduinoOTA.onStart([]() {
    Serial.println("Début de la mise à jour OTA...");
    pixels.setPixelColor(0, pixels.Color(255, 0, 255)); // Magenta pendant OTA
    pixels.show();
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nFin de la mise à jour OTA.");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progression OTA: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.begin();
  
  server.on("/", HTTP_GET, [](){
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    if (isAPMode) {
      server.send_P(200, "text/html", setup_html);
    } else {
      server.send_P(200, "text/html", index_html);
    }
  });
  
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
    
    // Télémétrie Onocoy
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
    
    // Nettoyage des vieux sats et ajout au json
    json += "\"skyplot\":[";
    skyplotSats.erase(std::remove_if(skyplotSats.begin(), skyplotSats.end(), [](const Satellite& s) {
        return millis() - s.lastSeen > 15000; // Oublier après 15s
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
  
  server.on("/nmea", HTTP_GET, [](){
    LOCK_GNSS();
    String out = "";
    for(auto& s : nmeaConsole) out += s + "\n";
    UNLOCK_GNSS();
    server.send(200, "text/plain", out);
  });
  
  server.on("/reboot", HTTP_POST, [](){
    Serial.println("Reboot manuel demande depuis l'interface web.");
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });
  
  // Web OTA
  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    String html = "<html><body style='background:#121212;color:white;font-family:sans-serif;text-align:center;padding:50px;'>";
    html += "<h2 style='color:#00ADB5;'>Mise a jour Web (OTA HTTP)</h2>";
    html += "<p style='color:#888;'>Version actuelle : v" + String(FIRMWARE_VERSION) + "</p>";
    html += "<p>Selectionnez le fichier firmware (ex: BASERTK.ino.bin)</p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='update' accept='.bin' style='margin:20px; padding:10px; background:#2b2b2b; color:white;'><br>";
    html += "<input type='submit' value='Flasher la carte' style='padding:15px 30px; background:#00ADB5; color:black; font-weight:bold; border:none; border-radius:5px; cursor:pointer;'>";
    html += "</form></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    bool hasError = Update.hasError();
    String msg = hasError ? "ECHEC de la mise a jour ! Veuillez verifier le fichier." : "SUCCES ! Redemarrage de l'antenne en cours...";
    String html = "<html><body style='background:#121212;color:white;font-family:sans-serif;text-align:center;padding:50px;'>";
    html += "<h2 style='color:" + String(hasError ? "#e74c3c" : "#2ecc71") + ";'>" + msg + "</h2>";
    html += "<p><a href='/' style='color:#00ADB5;'>Retourner a l'accueil</a></p>";
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
      Serial.printf("Debut Web OTA: %s\n", upload.filename.c_str());
      pixels.setPixelColor(0, pixels.Color(0, 255, 255)); // Cyan pendant Web OTA
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
        Serial.printf("Web OTA Succes: %u bytes\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.on("/start_svin", HTTP_POST, [](){
    Serial.println("Lancement Manuel du Survey-In demandé !");
    // Activer l'envoi régulier du statut et les phrases NMEA nécessaires
    computeAndSendChecksum("PQTMCFGMSGRATE,W,PQTMSVINSTATUS,1,1");
    delay(200);
    computeAndSendChecksum("PAIR062,0,1"); // Active GGA (1Hz)
    delay(200);
    computeAndSendChecksum("PAIR062,3,1"); // Active GSV (1Hz)
    delay(200);
    // Lancer le survey-in: 86400s (24h), précision 3.0m pour permettre le démarrage des observations
    computeAndSendChecksum("PQTMCFGSVIN,W,1,86400,3.0,0,0,0");
    delay(200);
    // Sauvegarder dans la mémoire NVRAM du module via PAIR
    computeAndSendChecksum("PAIR513");
    delay(200);
    // Rebooter le module Quectel via PAIR
    computeAndSendChecksum("PAIR023");
    
    preferences.putBool("hasSavedPos", false);
    
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });
  
  server.on("/test_quectel", HTTP_POST, [](){
    Serial.println("Test Quectel demandé !");
    computeAndSendChecksum("PQTMVERNO");
    delay(200);
    computeAndSendChecksum("PQTMCFGSVIN,R");
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/init_base", HTTP_POST, [](){
    Serial.println("Initialisation du mode Base demandée !");
    computeAndSendChecksum("PAIR514");
    delay(200);
    computeAndSendChecksum("PQTMCFGRCVRMODE,W,2");
    delay(200);
    computeAndSendChecksum("PAIR062,0,1"); // Active GGA (1Hz)
    delay(200);
    computeAndSendChecksum("PAIR062,3,1"); // Active GSV (1Hz)
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

  server.on("/fix_base", HTTP_POST, [](){
    LOCK_GNSS();
    String x = svinMeanX;
    String y = svinMeanY;
    String z = svinMeanZ;
    UNLOCK_GNSS();
    
    Serial.println("Fixation de la base demandee avec :");
    Serial.printf("X: %s, Y: %s, Z: %s\n", x.c_str(), y.c_str(), z.c_str());
    
    if (x != "0.0000" && x != "") {
      // Mode 2 : mode Fixed Base avec coordonnées ECEF fournies
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
      server.send(400, "text/plain", "Coordonnees invalides");
    }
  });

  server.on("/clear_position", HTTP_POST, [](){
    Serial.println("Effacement de la position enregistree en Flash demande...");
    preferences.putBool("hasSavedPos", false);
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });
  
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
      
      Serial.println("Nouvelle configuration Onocoy enregistree en NVS :");
      Serial.println("Host: " + host + ", Mount: " + mount);
      
      server.send(200, "text/plain", "OK");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Champs manquants");
    }
  });

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
      response += "<h2 style='color:#2ecc71;'>Configuration enregistree avec succes !</h2>";
      response += "<p>Redemarrage de la base en cours... Elle tentera de se connecter au reseau Wi-Fi : <b>" + ssid_val + "</b></p>";
      response += "</body></html>";
      server.send(200, "text/html", response);
      
      delay(2000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Champs requis manquants");
    }
  });

  server.on("/save_wifi", HTTP_POST, [](){
    if (server.hasArg("wifi_ssid") && server.hasArg("wifi_pass")) {
      String ssid_val = server.arg("wifi_ssid");
      String pass_val = server.arg("wifi_pass");
      
      ssid_val.trim();
      pass_val.trim();
      
      preferences.putString("wifi_ssid", ssid_val);
      preferences.putString("wifi_pass", pass_val);
      
      Serial.println("Nouvelle configuration WiFi enregistree en NVS :");
      Serial.println("SSID: " + ssid_val);
      
      server.send(200, "text/plain", "OK");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/plain", "Champs manquants");
    }
  });

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
        preferences.putBool("hasSavedPos", true);
        
        Serial.println("Nouvelles coordonnees de reference enregistrees en NVS :");
        Serial.printf("X: %s, Y: %s, Z: %s\n", x_val.c_str(), y_val.c_str(), z_val.c_str());
        
        server.send(200, "text/plain", "OK");
        delay(1000);
        ESP.restart();
      } else {
        server.send(400, "text/plain", "Champs invalides");
      }
    } else {
      server.send(400, "text/plain", "Champs manquants");
    }
  });
  
  server.begin();
  Serial.println("Serveur Web HTTP demarre sur le port 80.");
  
  xTaskCreate(internetCheckTask, "InternetCheck", 2048, NULL, 1, NULL);

  caster.begin();
  Serial.println("Caster TCP demarre sur le port " + String(CASTER_PORT));

  // Lancement de la tâche agressive GNSS sur le cœur 0 (PRO_CPU) avec priorité de 5
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

void gnssDataPumpTask(void * pvParameters) {
  uint8_t buffer[1024];
  static CustomWiFiClient onocoyClient;
  onocoyClient.setTimeout(50);
  static unsigned long lastOnocoyAttempt = 0;

  for (;;) {
    // Machine à états asynchrone et non-bloquante pour Onocoy
    if (WiFi.status() == WL_CONNECTED) {
      if (!onocoyConnected && !onocoyClient.connected()) {
        if (millis() - lastOnocoyAttempt > 10000) {
          lastOnocoyAttempt = millis();
          if (!onocoyIpResolved || onocoyIP == IPAddress(0,0,0,0)) {
            logOnocoy("Connexion impossible : IP Onocoy non resolue.");
          } else {
            logOnocoy("Connexion à Onocoy (" + onocoyIP.toString() + ")...");
            if (onocoyClient.connect(onocoyIP, onocoy_port)) {
              // Envoi de la poignée de main NTRIP Source standard
              onocoyClient.print("SOURCE ");
              onocoyClient.print(onocoy_password);
              onocoyClient.print(" /");
              onocoyClient.print(onocoy_mountpoint);
              onocoyClient.print("\r\n");
              onocoyClient.print("Source-Agent: ESP32-S3-RTK/1.3.24\r\n");
              onocoyClient.print("\r\n");
              onocoyClient.flush();
              
              // Attente de la réponse (max 1500ms)
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
                logOnocoy("Handshake Onocoy REUSSI. Minage actif !");
              } else {
                logOnocoy("Echec Handshake Onocoy. Reponse: " + response);
                onocoyClient.stop();
              }
            } else {
              logOnocoy("Echec de la connexion TCP a Onocoy.");
              onocoyClient.stop();
            }
          }
        }
      } else if (onocoyConnected && !onocoyClient.connected()) {
        LOCK_GNSS();
        onocoyConnected = false;
        UNLOCK_GNSS();
        logOnocoy("Deconnexion de servers.onocoy.com.");
      }
    } else {
      if (onocoyConnected || onocoyClient.connected()) {
        LOCK_GNSS();
        onocoyConnected = false;
        UNLOCK_GNSS();
        onocoyClient.stop();
        logOnocoy("Perte WiFi : deconnexion Onocoy.");
      }
    }

    // 1. Détection et acceptation des clients caster TCP (NTRIP handshake non bloquant)
    if (caster.hasClient()) {
      WiFiClient newClient = caster.available();
      if (newClient) {
        newClient.setTimeout(10);
        Serial.printf("\nNouveau client Caster connecte: %s\n", newClient.remoteIP().toString().c_str());
        
        // Attendre jusqu'à 50ms pour les en-têtes NTRIP
        int delayCount = 0;
        while (!newClient.available() && delayCount < 10) {
          vTaskDelay(5 / portTICK_PERIOD_MS);
          delayCount++;
        }
        
        if (newClient.available()) {
          String request = newClient.readStringUntil('\n');
          if (request.startsWith("GET /") || request.startsWith("GET ")) {
            newClient.print("ICY 200 OK\r\n\r\n");
            newClient.flush();
            Serial.println("Handshake NTRIP envoye: ICY 200 OK");
          }
        }
        
        LOCK_GNSS();
        clients.push_back({newClient, millis()});
        lastClientFlash = millis();
        UNLOCK_GNSS();
      }
    }
    
    LOCK_GNSS();
    for (int i = clients.size() - 1; i >= 0; i--) {
      if (!clients[i].client.connected()) {
        Serial.println("\nClient deconnecte.");
        clients.erase(clients.begin() + i);
      }
    }
    UNLOCK_GNSS();

    // 2. Lecture agressive du port Serial1
    size_t bytesRead = 0;
    while (Serial1.available() && bytesRead < sizeof(buffer)) {
      buffer[bytesRead++] = Serial1.read();
    }

    // 3. Envoi instantané aux clients et transfert au parseur NMEA (thread-safe)
    if (bytesRead > 0) {
      // Copie locale des clients actifs sous verrou pour éviter de garder le verrou pendant les écritures TCP
      std::vector<WiFiClient> activeClients;
      LOCK_GNSS();
      for (size_t i = 0; i < clients.size(); i++) {
        if (clients[i].client.connected()) {
          activeClients.push_back(clients[i].client);
        }
      }
      UNLOCK_GNSS();

      // Envoi aux clients caster en dehors du verrou et de manière non-bloquante
      for (size_t i = 0; i < activeClients.size(); i++) {
        if (activeClients[i].connected()) {
          activeClients[i].write(buffer, bytesRead);
        }
      }

      // Parse NMEA sous verrou
      LOCK_GNSS();
      for (size_t i = 0; i < bytesRead; i++) {
        char c = (char)buffer[i];
        // Filtre d'entrée : caractères ASCII imprimables classiques + CR + LF
        bool isPrintable = (c >= 32 && c <= 126) || c == '\r' || c == '\n';
        if (!isPrintable) {
          continue; // Pas un caractère NMEA valide, ignorer
        }
        
        // Si on n'est pas actif dans une phrase NMEA et que ce n'est pas le début '$', on ignore
        if (currentNMEA.length() == 0 && c != '$') {
          continue;
        }
        processNMEAByte(c);
      }
      bool sendToOnocoy = onocoyConnected;
      UNLOCK_GNSS();

      // Envoi à Onocoy en dehors du verrou et de manière non-bloquante avec vérification de buffer disponible
      if (sendToOnocoy && onocoyClient.connected()) {
        if (onocoyClient.availableForWrite() >= bytesRead) {
          onocoyClient.write(buffer, bytesRead);
          LOCK_GNSS();
          onocoyPacketsSent++;
          UNLOCK_GNSS();
        }
      }
    }

    vTaskDelay(2 / portTICK_PERIOD_MS);
  }
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  updateLED();
  
  static unsigned long lastQuectelConfig = 0;
  if (millis() - lastQuectelConfig > 900000) { // Toutes les 15 minutes
    lastQuectelConfig = millis();
    // Confirmer périodiquement l'activation des phrases NMEA indispensables (GGA et GSV)
    computeAndSendChecksum("PAIR062,0,1"); // Active GGA (1Hz)
    delay(50);
    computeAndSendChecksum("PAIR062,3,1"); // Active GSV (1Hz)
    delay(50);
  }
  
  delay(1);
}
