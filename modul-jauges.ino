#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // Indispensable pour lire les ordres
#include "ESPTelnet.h"
#include "HX711.h"
#include <Preferences.h>

// --- CONFIGURATION RÉSEAU ---
const char* ssid = "CarsNet_Internal_Modul";
const char* pass = "AdminCars123Esp32"; 

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
ESPTelnet telnet;

// --- VARIABLES ---
unsigned long lastUpdate = 0;
const float vRef = 3.3;
float calibrationScale = 31.9; 

// Variables de bouteille (en grammes pour plus de précision avec HX711)
float pVide = 13000.0;  // Par défaut 13kg
float pPlein = 26000.0; // Par défaut 26kg (13kg gaz)

void addToLog(String msg) {
  String entry = "[" + String(millis() / 1000) + "s] " + msg;
  Serial.println(entry);
  if (telnet.isConnected()) telnet.println(entry);
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

  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  MDNS.begin("modul-jauges");
  telnet.begin();
  
  addToLog("Modul-Jauges Synchro. IP: " + WiFi.localIP().toString());
}

void loop() {
  telnet.loop();
  if (WiFi.status() == WL_CONNECTED && millis() - lastUpdate > 10000) {
    echangerDonnees(); // Remplace envoyerDonnees
    lastUpdate = millis();
  }
}
