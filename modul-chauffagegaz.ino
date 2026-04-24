#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h> // À installer via le gestionnaire de bibliothèques

// --- CONFIGURATION ---
const char* ssid = "CarsNet_Internal_Modul";
const char* pass = "AdminCars123Esp32";
const int pinRelais = 5; // Ton relais sur GPIO 5

Adafruit_ADS1115 ads;
ESP8266WebServer server(80);
WiFiClient wifiClient;

int etatChauffageGaz = 0;
int dernierEtatFlamme = -1;
unsigned long lastSync = 0;

void addToLog(String msg) {
    Serial.println("[" + String(millis()/1000) + "s] " + msg);
}

// --- LOGIQUE THERMOCOUPLE ---
int detecterFlamme() {
    int16_t results = ads.readADC_SingleEnded(0);
    float mv = ads.computeVolts(results) * 1000; // Conversion en millivolts

    // Un thermocouple produit ~10-25mV en fonctionnement.
    // On considère la flamme présente au dessus de 5mV.
    return (mv > 5.0) ? 1 : 0;
}

// --- COMMUNICATION THERMOSTAT ---
void syncThermostat() {
    
    String targetIP = "192.168.4.1";

    HTTPClient http;
    
    // 1. On récupère l'ordre de chauffage
    http.begin(wifiClient, "http://" + targetIP + "/status");
    int httpCode = http.GET();

    if (httpCode == 200) {
        String payload = http.getString();
        addToLog("Payload reçu : " + payload);
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        
        // CORRECTION : On utilise la clé exacte retournée par le thermostat
        // D'après ta chaîne : {"chauffepetrole": 0, "chauffegaz": 1, ...}
        if (doc.containsKey("chauffegaz")) {
            int ordre = doc["chauffegaz"].as<int>(); 
            
            if (ordre != etatChauffageGaz) {
                etatChauffageGaz = ordre;
                
                // Sur ton module ESP8266 Relay X1 : 
                // HIGH = Relais collé (ON), LOW = Relais repos (OFF)
                digitalWrite(pinRelais, (etatChauffageGaz == 1) ? HIGH : LOW);
                
                addToLog("Ordre reçu : " + String(etatChauffageGaz ? "DEMARRAGE" : "ARRET"));
            }
        }
    } else {
        addToLog("Erreur HTTP : " + String(httpCode));
    }

    // 2. On envoie l'état de la flamme (Thermocouple)
    int flammeActuelle = detecterFlamme();
    if (flammeActuelle != dernierEtatFlamme) {
        http.begin(wifiClient, "http://" + targetIP + "/updateflamme?val=" + String(flammeActuelle));
        if (http.GET() == 200) {
            dernierEtatFlamme = flammeActuelle;
            addToLog("🔥 Statut Flamme envoyé : " + String(flammeActuelle));
        }
    }
    http.end();
}

void setup() {
    Serial.begin(115200);
    pinMode(pinRelais, OUTPUT);
    digitalWrite(pinRelais, LOW);

    // Initialisation I2C sur SDA=4, SCL=5
    Wire.begin(4, 5);
    
    if (!ads.begin()) {
        addToLog("Erreur: ADS1115 non trouvé !");
    }
    // Gain maximum pour capter les millivolts du thermocouple
    ads.setGain(GAIN_SIXTEEN); 

    WiFi.begin(ssid, pass);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }

    MDNS.begin("modul-chauffagegaz");
    server.begin();
    addToLog("Modul Gaz prêt sur IP: " + WiFi.localIP().toString());
}

void loop() {
    server.handleClient();
    MDNS.update();

    if (millis() - lastSync > 5000) { // Sync toutes les 5 secondes
        syncThermostat();
        lastSync = millis();
    }
}
