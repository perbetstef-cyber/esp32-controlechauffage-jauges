#include "WiFi.h"
#include "Preferences.h"
#include "WebServer.h"
#include "ESPTelnet.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>


WebServer server(80);
ESPTelnet telnet;

// --- CONFIGURATION PINS ---
const int pinChauffe  = 27; // Sortie 1 (Chauffage liquide)
const int pinTurbine  = 16; // Sortie 4 (Turbine PWM)
const int pinSondeEau = 25; // Entrée sonde eau chaude

// --- PARAMÈTRES PWM ---
const int freq = 5000;
const int resolution = 8; // 0 à 255

int SpinChauffe = 0;
int SVitesseTurbine = 0; 

String ssid, pass;
int dernierEtatEau = -1;
unsigned long lastCheck = 0;

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

  pinMode(pinSondeEau, INPUT_PULLDOWN);
  
  
  ssid = "CarsNet_Internal_Modul";
  pass = "AdminCars123Esp32";

  attemptConnection();

}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
    telnet.loop();
    if (millis() - lastCheck > 5000) {
      recupererOrdres();
      surveillerEauChaude();
      lastCheck = millis();
    }
  }
}

String trouverIpThermostat() {
  return "192.168.4.1";
}

void appliquerVitesse(int vitesse) {
  SVitesseTurbine = vitesse;
  int pwmVal = 0;
  
  if (vitesse == 30)  pwmVal = 77;
  else if (vitesse == 60)  pwmVal = 153;
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
  int etatActuel = (digitalRead(pinSondeEau) == LOW) ? 1 : 0;
  if (etatActuel != dernierEtatEau) {
    HTTPClient http;
    String ip = trouverIpThermostat();
    if (ip != "") {
        String url = "http://"+ip+"/updatechauffagepetrole?mac=" + WiFi.macAddress() + "&val=" + String(etatActuel);
        http.begin(url);
        if (http.GET() == 200) {
          dernierEtatEau = etatActuel;
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
  if (WiFi.status() == WL_CONNECTED) {
    MDNS.begin("modul-chauffagepetrole");
    server.on("/", handleRoot);
    server.on("/relais1", handleR1);
    server.on("/turbine", handleTurbine);
    server.begin();
    telnet.begin();
    addToLog("\nWiFi OK : " + WiFi.localIP().toString());
  } else {
    WiFi.disconnect();
  }
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
  html += (digitalRead(pinSondeEau) == LOW) ? "CHAUDE (Actif)" : "FROIDE (Inactif)";
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
