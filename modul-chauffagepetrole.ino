#include "WiFi.h"
#include "Preferences.h"
#include "WebServer.h"
#include "ESPTelnet.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

WebServer server(80);
ESPTelnet telnet;

// --- CONFIGURATION PINS ---
const int pinChauffe  = 27; // Sortie 1 (Chauffage liquide)
const int pinTurbine  = 16; // Sortie 4 (Turbine PWM)

// --- PARAMÈTRES PWM ---
const int freq = 5000;
const int resolution = 8; // 0 à 255

int SpinChauffe = 0;
int SVitesseTurbine = 0; 
const float vRef = 3.3;

String ssid, pass;
String ota_pass = "dyqpa8we";
int dernierEtatEauT = -1;
unsigned long lastCheck = 0;

// SONDE THERMIQUE //
// --- CONFIGURATION PINS ---
const int pinVoltageSonde = 35; // Nouvelle pin pour la sonde (GPIO 35)

unsigned long dernierContactThermostat = 0; 
const unsigned long DELAI_MAX_THERMOSTAT = 60000; // 1 minutes en millisecondes (60 * 1000)

// --- TABLE DE CORRESPONDANCE (Tes relevés) ---
float convertirADCVersTemp(float adc) {
  if (adc < 200 || adc > 4000) return 00.0;

  if (adc >= 1730) return 32.0;
  if (adc >= 1615) return 35.0;
  if (adc >= 1540) return 40.0;
  if (adc >= 1480) return 45.0;
  if (adc >= 1380) return 50.0;
  if (adc >= 1300) return 55.0;
  if (adc >= 1210) return 60.0;
  if (adc >= 1125) return 65.0;
  if (adc >= 1035) return 70.0;
  if (adc >= 920)  return 75.0;
  if (adc >= 850)  return 80.0;
  if (adc >= 800)  return 85.0;
  if (adc <= 750)  return 90.0;
  return 25.0; // Par défaut
}


// --- PROTOTYPES ---
void handleRoot();
void handleR1(); 
void handleTurbine();
void recupererOrdres();
void surveillerEauChaude();
void attemptConnection();
void startBluetooth();
void checkBTCommands();
String trouverIpThermostat();



void addToLog(String msg) {
  String entry = "[" + String(millis() / 1000) + "s] " + msg;
  Serial.println(entry);
  if (telnet.isConnected()) { 
    telnet.println(entry); 
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(pinChauffe, OUTPUT);
  digitalWrite(pinChauffe, LOW);

  // --- CONFIGURATION PWM (VERSION ESP32 3.0+) ---
  ledcAttach(pinTurbine, freq, resolution);
  ledcWrite(pinTurbine, 0); // Éteint au démarrage
  appliquerVitesse(0);

  ssid = "CarsNet_Internal";
  pass = "AdminCars123";

  dernierContactThermostat = millis();

  attemptConnection();

}

void loop() {
  ArduinoOTA.handle();
  telnet.loop();
  server.handleClient();

  // --- SÉCURITÉ : VÉRIFICATION DU TIMEOUT THERMOSTAT ---
  if (SpinChauffe == 1 && (millis() - dernierContactThermostat > DELAI_MAX_THERMOSTAT)) {
    addToLog("⚠️ ALERTE : Pas de nouvelles du thermostat depuis 10 min ! Arrêt de sécurité.");
    SpinChauffe = 0;
    digitalWrite(pinChauffe, LOW);  // On coupe le chauffage
    appliquerVitesse(0);            // On coupe la ventilation
  }

  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 10000) { // Tentative toutes les 10s
      addToLog("WiFi perdu, reconnexion...");
      WiFi.reconnect();
      lastReconnectAttempt = millis();
    }
    return; // On ne fait pas le reste si pas de WiFi
  }

  if (millis() - lastCheck > 5000) {
      recupererOrdres();
      surveillerEauChaude();
      lastCheck = millis();
    }
}

String trouverIpThermostat() {
  return "192.168.4.1";
}

void appliquerVitesse(int vitesse) {
  SVitesseTurbine = vitesse;
  int pwmVal = 0;
  
  if (vitesse == 30)  pwmVal = 155;
  else if (vitesse == 60)  pwmVal = 202;
  else if (vitesse == 100) pwmVal = 255;
  else pwmVal = 0;

  ledcWrite(pinTurbine, pwmVal);
  addToLog("Turbine : " + String(vitesse) + "%");
}

void recupererOrdres() {
  String ip = trouverIpThermostat();
  if (ip == "") return;

  HTTPClient http;
  http.begin("http://" + ip + "/status"); 
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    dernierContactThermostat = millis();
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);

    int chauffe = doc["chauffepetrole"]; 
    SpinChauffe = chauffe;
    digitalWrite(pinChauffe, (chauffe == 1) ? HIGH : LOW);

    int v = doc["vitesseventil"]; // Reçoit 0, 1, 2 ou 3
    if (v == 0) appliquerVitesse(0);
    else if (v == 1) appliquerVitesse(30);
    else if (v == 2) appliquerVitesse(60);
    else if (v == 3) appliquerVitesse(100);
  }
  http.end();
}

void surveillerEauChaude() {
  
  long somme = 0;
  for (int i = 0; i < 50; i++) { somme += analogRead(pinVoltageSonde); delay(2); }
  float moyADC = (float)somme / 50.0;
  addToLog("DEBUG ADC BRUT = " + String(moyADC));
  int tempRelle = (int)convertirADCVersTemp(moyADC);
  addToLog("Température Eau : " + String(tempRelle) + "°C");

  if (tempRelle != dernierEtatEauT) {
    HTTPClient http;
    String ip = trouverIpThermostat();
    if (ip != "") {
        // On utilise le paramètre 'val' pour passer la température en degrés
        String url = "http://"+ip+"/updatechauffagepetroletemperature?mac=" + WiFi.macAddress() + "&val=" + String(tempRelle);
        http.begin(url);
        if (http.GET() == 200) {
          dernierEtatEauT = tempRelle;
        }
        http.end();
    }
  }

  
}

void attemptConnection() {
  WiFi.setHostname("MODUL-CHAUFFAGEPETROLE");
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  
    if (MDNS.begin("modul-chauffagepetrole")) {
    MDNS.addService("http", "tcp", 80);
  }
    server.on("/", handleRoot);
    server.on("/relais1", handleR1);
    server.on("/turbine", handleTurbine);
    server.begin();
    telnet.begin();
    telnet.onConnect([](String ip) {
    addToLog("\nBienvenue sur le Modul-Chauffagepetrole !");
  });
    addToLog("\nWiFi OK : " + WiFi.localIP().toString());
  
  ArduinoOTA.setHostname("modul-chauffagepetrole");
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
}

void handleR1() {
  SpinChauffe = server.arg("val").toInt();
  digitalWrite(pinChauffe, (SpinChauffe == 1) ? HIGH : LOW);
  server.send(200, "text/plain", "OK");
}

void handleTurbine() {
  int val = server.arg("val").toInt(); // Reçoit le % (0, 30, 60, 100)
  appliquerVitesse(val);
  server.send(200, "text/plain", "OK");
}





void handleRoot() {
  String html = "<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:sans-serif; background:#f4f4f4; padding:20px;} .card{background:white; padding:15px; border-radius:10px; box-shadow:0 2px 5px rgba(0,0,0,0.1); margin-bottom:15px;} ";
  html += "a{text-decoration:none; background:#3498db; color:white; padding:5px 10px; border-radius:5px; margin-right:5px;} .off-btn{background:#e74c3c;}</style></head><body>";
  
  html += "<h1>🏠 MODUL-CHAUFFAGE (VARIATEUR)</h1>";

  html += "<div class='card'><h2>🔥 Eau Chaude (Sonde)</h2>";
  long somme = 0;
  for (int i = 0; i < 50; i++) { somme += analogRead(pinVoltageSonde); delay(2); }
  float moyADC = (float)somme / 50.0;
  int tempRelle = (int)convertirADCVersTemp(moyADC);
  html += " température : "+String(tempRelle)+"°C";
  html += "</div>";

  html += "<div class='card'><h2>Chauffage Liquide (Pin 27)</h2>";
  html += (SpinChauffe == 1) ? "ÉTAT : ALLUMÉ" : "ÉTAT : ÉTEINT";
  html += "<br><br><a href='/relais1?val=1'>ON</a><a href='/relais1?val=0' class='off-btn'>OFF</a></div>";

  html += "<div class='card'><h2>Turbine Variateur (Pin 16)</h2>";
  html += "PUISSANCE : " + String(SVitesseTurbine) + "%";
  html += "<br><br><a href='/turbine?val=0' class='off-btn'>OFF</a>";
  html += "<a href='/turbine?val=30'>30%</a>";
  html += "<a href='/turbine?val=60'>60%</a>";
  html += "<a href='/turbine?val=100'>100%</a></div>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}
