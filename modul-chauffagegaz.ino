#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>

// --- CONFIGURATION ---
const char* ssid = "CarsNet_Internal";
const char* pass = "AdminCars123";
const int pinRelais = 27; // Pin GPIO au choix sur ESP32 (ex: 27)

Adafruit_ADS1115 ads;
WebServer server(80);
// Pas besoin de WiFiClient explicite pour HTTPClient sur ESP32

int etatChauffageGaz = 0;
int dernierEtatFlamme = -1;
unsigned long lastSync = 0;

void addToLog(String msg) {
    Serial.println("[" + String(millis()/1000) + "s] " + msg);
}

// --- LOGIQUE THERMOCOUPLE ---
int detecterFlamme() {
    int16_t results = ads.readADC_SingleEnded(0);
    float mv = ads.computeVolts(results) * 1000;
    return (mv > 5.0) ? 1 : 0;
}

// --- COMMUNICATION THERMOSTAT ---
void syncThermostat() {
    // Sur ESP32, l'IP fixe du Thermostat/Routeur est 192.168.4.1
    String targetIP = "192.168.4.1"; 

    HTTPClient http;
    
    // 1. RÉCUPÉRATION DES ORDRES
    http.begin("http://" + targetIP + "/status");
    int httpCode = http.GET();

    if (httpCode == 200) {
        String payload = http.getString();
        // Décommenter pour voir la chaîne brute dans les logs
        // addToLog("Payload reçu : " + payload); 

        DynamicJsonDocument doc(1024);
        deserializeJson(doc, payload);
        
        if (doc.containsKey("chauffegaz")) {
            int ordre = doc["chauffegaz"].as<int>(); 
            if (ordre != etatChauffageGaz) {
                etatChauffageGaz = ordre;
                // Vérifie si ton relais ESP32 est Active High ou Low
                digitalWrite(pinRelais, (etatChauffageGaz == 1) ? HIGH : LOW);
                addToLog("Relais GAZ -> " + String(etatChauffageGaz ? "ON" : "OFF"));
            }
        }
    } else {
        addToLog("Erreur Thermostat (192.168.4.1) : " + String(httpCode));
    }

    // 2. ENVOI DU STATUT FLAMME
    int flammeActuelle = detecterFlamme();
    if (flammeActuelle != dernierEtatFlamme) {
        http.begin("http://" + targetIP + "/updatechauffagegaz?val=" + String(flammeActuelle));
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

    // Initialisation I2C sur ESP32 (SDA=21, SCL=22 par défaut)
    Wire.begin(21, 22);
    
    if (!ads.begin()) {
        addToLog("Erreur: ADS1115 non trouvé !");
    }
    ads.setGain(GAIN_SIXTEEN); 

    WiFi.begin(ssid, pass);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    if (MDNS.begin("modul-chauffagegaz")) {
        addToLog("mDNS Responder démarré");
    }

    server.begin();
    addToLog("Modul Gaz ESP32 prêt sur IP: " + WiFi.localIP().toString());
}

void loop() {
    server.handleClient();
    // Sur ESP32, MDNS.update() n'est pas nécessaire (géré par tâche de fond)

    if (millis() - lastSync > 5000) {
        syncThermostat();
        lastSync = millis();
    }
}
