#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // Indispensable pour lire les ordres
#include "ESPTelnet.h"
#include "HX711.h"
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "WebServer.h"

WebServer server(80);
ESPTelnet telnet;

// --- CONFIGURATION PINS ---
const int pinVoltage = 34;
const int pinPniv1 = 12; 
const int pinPniv2 = 14; 
const int pinPniv3 = 27; 
const int pinPniv4 = 26; 
const int LOADCELL_DOUT_PIN = 5; 
const int LOADCELL_SCK_PIN = 4;

// --- OBJETS ---
HX711 scale;
Preferences preferences;

// --- VARIABLES ---
unsigned long lastUpdate = 0;
const float vRef = 3.3;
float calibrationScale = 31.9; 
String ota_pass = "dyqpa8we";

// Variables de bouteille (en grammes pour plus de précision avec HX711)
float pVide = 13000.0;  // Par défaut 13kg
float pPlein = 26000.0; // Par défaut 26kg (13kg gaz)
String ssid, pass;

void addToLog(String msg) {
  String entry = "[" + String(millis() / 1000) + "s] " + msg;
  Serial.println(entry);
  if (telnet.isConnected()) { 
    telnet.println(entry); 
  }
}

// --- LOGIQUE TENSION ---
float lireVoltage() {
  long somme = 0;
  for (int i = 0; i < 50; i++) { somme += analogRead(pinVoltage); delay(1); }
  float vBrute = ((float)somme / 50 * vRef / 4095.0) * 5.0;
  return (vBrute >= 10.0) ? (0.884 * vBrute) + 1.83 : vBrute;
}

// --- LOGIQUE GAZ (%) ---
int lireNiveauGaz() {
  if (scale.is_ready()) {
    float poidsActuel = scale.get_units(10); 
    float chargeUtile = pPlein - pVide;
    if (chargeUtile <= 0) return 0;

    int pourcentage = ((poidsActuel) / chargeUtile) * 100.0;
    
    if (pourcentage > 100) pourcentage = 100;
    if (pourcentage < 0) pourcentage = 0;
    
    return pourcentage;
  }
  return 0;
}

int lireNiveauPetrole() {
  if (digitalRead(pinPniv4) == LOW) return 4;
  if (digitalRead(pinPniv3) == LOW) return 3;
  if (digitalRead(pinPniv2) == LOW) return 2;
  if (digitalRead(pinPniv1) == LOW) return 1;
  return 0;
}

// --- COMMUNICATION (ÉCHANGE COMPLET) ---
void echangerDonnees() {
  String ip = "192.168.4.1";
  HTTPClient http;

  // 1. RÉCUPÉRATION DES ORDRES (PULL)
  http.begin("http://" + ip + "/getGazOrders");
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<300> doc;
    deserializeJson(doc, payload);

    bool actionFaite = false;

    // Ordre de Tare ?
    if (doc["tare"] == 1) {
      addToLog("Ordre distant : Faire la Tare...");
      scale.tare(20);
      preferences.begin("jauge", false);
      preferences.putLong("tare", scale.get_offset());
      preferences.end();
      actionFaite = true;
    }

    // Ordre de Config Bouteille ?
    if (doc.containsKey("pvide") && (float)doc["pvide"] != -1.0) {
      pVide = (float)doc["pvide"] * 1000.0;  // conversion kg -> g
      pPlein = (float)doc["pplein"] * 1000.0; 
      preferences.begin("jauge", false);
      preferences.putFloat("pvide", pVide);
      preferences.putFloat("pplein", pPlein);
      preferences.end();
      addToLog("Nouveau setup bouteille : " + String(pVide/1000.0) + "kg vide");
      addToLog("Nouveau setup bouteille : " + String(pPlein/1000.0) + "kg pleine");
      actionFaite = true;
    }

    // Si on a bossé, on demande au thermostat d'effacer l'ordre
    if (actionFaite) {
      http.begin("http://" + ip + "/clearGazOrder");
      http.GET();
    }
  }

  // 2. ENVOI DES MESURES (PUSH)
  float volt = lireVoltage();
  int petrole = lireNiveauPetrole();
  int gaz = lireNiveauGaz();
  
  http.begin("http://" + ip + "/updatepetrole?val=" + String(petrole)); http.GET();
  http.begin("http://" + ip + "/updatevolt?val=" + String(volt, 2)); http.GET();
  http.begin("http://" + ip + "/updategaz?val=" + String(gaz)); http.GET();
  
  http.end();
  addToLog("Cycle OK -> " + String(volt, 2) + "V | Pétrole: " + String(petrole) + " | Gaz: " + String(gaz) + "%");
}

void setup() {
  Serial.begin(115200);

  pinMode(pinPniv1, INPUT_PULLUP);
  pinMode(pinPniv2, INPUT_PULLUP);
  pinMode(pinPniv3, INPUT_PULLUP);
  pinMode(pinPniv4, INPUT_PULLUP);

  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  
  // Chargement des paramètres depuis la Flash
  preferences.begin("jauge", true);
  scale.set_offset(preferences.getLong("tare", 0));
  pVide = preferences.getFloat("pvide", 13000.0);
  pPlein = preferences.getFloat("pplein", 26000.0);
  preferences.end();
  
  scale.set_scale(calibrationScale);

  ssid = "CarsNet_Internal";
  pass = "AdminCars123";

  WiFi.setHostname("MODUL-JAUGES");
  WiFi.begin(ssid.c_str(), pass.c_str());
 while (WiFi.status() != WL_CONNECTED ) {
    delay(500);
    Serial.print(".");
  }
  
  telnet.begin();
  telnet.onConnect([](String ip) {
    addToLog("\nBienvenue sur le Modul-Jauges !");
  });

  // 3. MDNS
  if (MDNS.begin("modul-jauges")) {
    MDNS.addService("http", "tcp", 80);
  }
  
  // 4. OTA Android / ArduinoOTA avec password
  ArduinoOTA.setHostname("modul-jauges");
  ArduinoOTA.setPassword(ota_pass.c_str());

  ArduinoOTA.onStart([]() {
    addToLog("OTA START");
  });

  ArduinoOTA.onEnd([]() {
    addToLog("OTA END");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress * 100) / total);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    addToLog("OTA ERROR : " + String((int)error));
  });

  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.begin();

  addToLog("Modul-Jauges Synchro. IP: " + WiFi.localIP().toString());
}

void loop() {
  // 1. Gérer les services (Priorité haute)
  ArduinoOTA.handle();
  telnet.loop();
  server.handleClient(); // INDISPENSABLE pour le HTTP
  
  // 2. Gestion de la reconnexion automatique
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 10000) { // Tentative toutes les 10s
      addToLog("WiFi perdu, reconnexion...");
      WiFi.reconnect();
      lastReconnectAttempt = millis();
    }
    return; // On ne fait pas le reste si pas de WiFi
  }

  // 3. Échange de données (Non-bloquant pour le reste)
  if (millis() - lastUpdate > 10000) {
    echangerDonnees();
    lastUpdate = millis();
  }
}

void handleRoot() {
  float volt = lireVoltage();
  int petrole = lireNiveauPetrole();
  int gaz = lireNiveauGaz();
  String html = "<html><head><meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#121212; color:#eee; font-family:sans-serif; margin:0; padding-bottom:80px;} ";
  html += ".nav{display:flex; justify-content:space-around; background:#1e1e1e; padding:15px; position:fixed; bottom:0; width:100%; border-top:1px solid #333;} ";
  html += ".nav a{color:#888; text-decoration:none; font-size:0.8em; font-weight:bold;} .page{display:none; padding:20px;} .page.active{display:block;} ";
  html += ".card{background:#1e1e1e; padding:15px; border-radius:10px; margin-bottom:15px; border:1px solid #333; border-left:4px solid #2196f3;} ";
  html += "input,select,button{display:block; width:100%; margin:10px 0; padding:12px; border-radius:5px; border:none; background:#222; color:white;} ";
  html += "button{background:#2196f3; font-weight:bold;} .status{display:inline-block; width:12px; height:12px; border-radius:50%; margin:0px 8px;} ";
  html += ".on{background:#2ecc71;} .off{background:#e74c3c;}</style></head><body>";

html += "<div id='p-dash' class='page active'><h1>Caravane Dashboard</h1>";

  html += "<div class='card'><b>Gazole :</b> " + String(petrole) + "/4</div>";
  html += "<div class='card'><b>Gaz :</b> " + String(gaz) + "%</div>";
  html += "<div class='card'><b>Volt :</b> " + String(volt) + "V</div>";
  html += "</div>";
server.send(200, "text/html", html);
}
