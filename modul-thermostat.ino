#include <WiFiClientSecure.h>
#include "WiFi.h"
#include "Preferences.h"
#include "WebServer.h"
#include "ESPTelnet.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>


WebServer server(80);
Preferences prefs;
ESPTelnet telnet;

// Pins entrées 
const int pinChauffage = 25;
const int pinVit1 = 35;
const int pinVit2 = 32;
const int pinVit3 = 33;
const int pinChauffeEauInput = 26; // L'interrupteur d'activation du chauffe eau
const int pinLedEauChaude = 27;     // Le négatif de la LED


String ext_ssid, ext_pass, api_url, kco_key;
unsigned long lastApiSync = 0;

struct RemoteOrders {
    int force_ch_gazole = -1; 
    int force_ch_gaz    = -1;
    int force_ch_eau    = -1;
} orders;

struct RemoteJauge {
  int levelPetrole;
  int levelGaz;
  float levelVolt;
  String label;
  long lastSeen;
};

struct MacChauffage {
  long lastSeenPetrole;
  long lastSeenGaz;
  String labelpetrole;
  String labelgaz;
  int levelChaleur;
  int levelFlame;
}; // <--- Ajout du ; ici

RemoteJauge Jauges = {0, 0, 0, "JAUGES RÉSERVOIR", 0};
MacChauffage Chauffages = {0,0,"Chauffage au pétrole","Chauffage au gaz",0,0}; // <--- Ajout du ; ici

// --- GESTION DES ORDRES GAZ ---
struct GazOrder {
    bool pendingTare = false;
    float pVide = 0;
    float pPlein = 0;
    bool pendingConfig = false;
} gazOrder;

// Nom pour Preferences
const char* PREF_GAZ = "gaz_mem";

// --- DÉCLARATION DES FONCTIONS ---
void handleRoot();
void handleUpdateJaugesPetrole();
void handleUpdateJaugesGaz();
void handleSaveMacJauges();
void handleSaveMacChauffages();
void handleUpdateChauffagePetrole();
void handleUpdateChauffageGaz();
void attemptConnection();
void startBluetooth();
void checkBTCommands();

void setup() {
  Serial.begin(115200);
  
  // Config des entrées
  pinMode(pinChauffage, INPUT_PULLDOWN);
  pinMode(pinVit1, INPUT_PULLDOWN);
  pinMode(pinVit2, INPUT_PULLDOWN);
  pinMode(pinVit3, INPUT_PULLDOWN);
  pinMode(pinChauffeEauInput, INPUT_PULLDOWN); // On attend le 3.3V de l'interrupteur
  pinMode(pinLedEauChaude, OUTPUT);
  digitalWrite(pinLedEauChaude, HIGH);

  prefs.begin("config", false);
  ext_ssid = prefs.getString("ssid", "");
  ext_pass = prefs.getString("pass", "");
  api_url  = prefs.getString("api", "https://carsadventures.fr/wp-json/api/v1/esp32/");
  kco_key  = prefs.getString("kco", "");

  // 1. WIFI CACHÉ pour l'interconnexion (Amélioration)
  WiFi.softAP("CarsNet_Internal_Modul", "AdminCars123Esp32", 1, 1); 

  // 2. WIFI EXTÉRIEUR
  if (ext_ssid != "") WiFi.begin(ext_ssid.c_str(), ext_pass.c_str());

  telnet.begin();
  telnet.onConnect([](String ip) {
    addToLog("\nBienvenue sur le Modul-Thermostat !");
  });

  // 3. MDNS (Amélioration)
  if (MDNS.begin("modul-thermostat")) MDNS.addService("http", "tcp", 80);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/updatepetrole", handleUpdateJaugesPetrole);
  server.on("/updategaz", handleUpdateJaugesGaz);
  server.on("/updatevolt", handleUpdateJaugesVolt);
  server.on("/updatechauffagegaz", handleUpdateChauffageGaz);
  server.on("/updatechauffagepetrole", handleUpdateChauffagePetrole);
  server.on("/status", handleStatus);
  
  server.on("/getGazOrders", HTTP_GET, []() {
      StaticJsonDocument<200> response;
      response["tare"] = gazOrder.pendingTare ? 1 : 0;
      
      if (gazOrder.pendingConfig) {
          response["pvide"] = gazOrder.pVide;
          response["pplein"] = gazOrder.pPlein;
      }

      String json;
      serializeJson(response, json);
      server.send(200, "application/json", json);
  });

  // Route de confirmation pour effacer la mémoire
  server.on("/clearGazOrder", HTTP_GET, []() {
      gazOrder.pendingTare = false;
      gazOrder.pendingConfig = false;
      sauverOrdreGaz(); // On vide la Flash
      server.send(200, "text/plain", "OK");
      addToLog("Ordre Gaz purgé (Jauge a récupéré)");
  });

  server.begin();
  addToLog("\nWiFi OK : " + WiFi.localIP().toString());
}

void loop() {
  server.handleClient();
  telnet.loop();

  // 1. Lire si l'utilisateur veut le chauffe-eau
  bool veutChauffeEau = (digitalRead(pinChauffeEauInput) == HIGH);

  // 2. Vérifier si l'eau est chaude (Info venant du module distant Pétrole)
  // On considère que levelChaleur == 1 signifie "Eau à température"
  long secContact = (millis() - Chauffages.lastSeenPetrole) / 1000;
  bool eauEstChaude = (Chauffages.levelChaleur >= 1 && secContact < 600);

  // 3. Piloter la LED (Le voyant "Eau Chaude" s'allume si l'eau est prête)
  // que le chauffage soit en route ou que l'interrupteur chauffe-eau soit ON
  if (eauEstChaude) {
    digitalWrite(pinLedEauChaude, LOW);  // Allume la LED (Négatif à la masse)
  } else {
    digitalWrite(pinLedEauChaude, HIGH); // Éteint la LED
  }

  if (millis() - lastApiSync > 10000) {
    syncWithAPI();
    lastApiSync = millis();
  }
}

bool petroleAutorise() {
    long sec = (millis() - Jauges.lastSeen) / 1000;
    bool online = (Jauges.lastSeen > 0 && sec < 600);
    if(online && Jauges.levelPetrole > 1 && Jauges.levelPetrole !=0 && Jauges.levelVolt > 12.1 ) { return true; }
    if(!online) { return true; }
    return false;
}
bool gazAutorise() {
    long sec = (millis() - Jauges.lastSeen) / 1000;
    bool online = (Jauges.lastSeen > 0 && sec < 600);
    if(online && Jauges.levelGaz > 1 && Jauges.levelGaz!=0 ) { return true; }
    if(!online) { return true; }
    return false;
}

// --- SYNCHRO API ---
void syncWithAPI() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (kco_key.length() == 0) {
    addToLog("KCO vide");
    return;
  }




  int activeChaufP = 0;
  int activeChaufE = 0;
  int activeChaufG = 0;
  int forceVentilP = 0;
  if(petroleAutorise()&&digitalRead(pinChauffage) == HIGH ) {
    activeChaufP = 1;
    if(Chauffages.levelChaleur>0) {
      if(digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
      if(digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
      if(digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
    }
  }
  else { 
      if(gazAutorise()&&digitalRead(pinChauffage) == HIGH ) {
      activeChaufG = 1;
      if(Chauffages.levelChaleur>0) {
        if(digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
        if(digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
        if(digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
      }
    } 
  }
  if(petroleAutorise()&&digitalRead(pinChauffeEauInput) == HIGH) {
    activeChaufE = 1;
  }
  if(orders.force_ch_gazole==1) {
    addToLog("force gazole detecté" );
    activeChaufP = 1;
  }
  if(orders.force_ch_eau==1) {
    addToLog("force eau detecté" );
    activeChaufE = 1;
  }
  if(orders.force_ch_gaz==1) {
    addToLog("force gaz detecté" );
    activeChaufG = 1;
  }



  DynamicJsonDocument doc(2048);
  JsonArray logs = doc.createNestedArray("logs");

  auto addLogJson = [&](const char* k, const String& v) {
    JsonObject l = logs.createNestedObject();
    if (l.isNull()) {
      addToLog("Erreur JSON: impossible d'ajouter l'objet log");
      return;
    }
    l["k"] = k;
    l["v"] = v;
  };
 
  addLogJson("st-ch-gazole", String(activeChaufP));
  addLogJson("st-ch-gaz", String(activeChaufG));
  addLogJson("st-chauffe-eau", String(activeChaufE));
  addLogJson("st-eau-chaude", String(Chauffages.levelChaleur));
  addLogJson("st-flamme", String(Chauffages.levelFlame));  
  
  addLogJson("niv-gazole", String(Jauges.levelPetrole));
  addLogJson("niv-gaz", String(Jauges.levelGaz));
  addLogJson("voltage", String(Jauges.levelVolt));

  addLogJson("ventilation", String(forceVentilP));

  String body;
  serializeJson(doc, body);

  String url = api_url;
  if (!url.endsWith("/")) url += "/";
  url += "?KCO=" + kco_key;

  WiFiClientSecure client;
  client.setInsecure(); // pour test; ensuite idéalement certificat

  HTTPClient http;
  if (!http.begin(client, url)) {
    addToLog("http.begin() a échoué");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.setUserAgent("ESP32-ModulThermostat/1.0");

  addToLog("POST URL: " + url);
  addToLog("Payload: " + body.substring(0, 250));

  int code = http.POST(body);
  addToLog("Code retour API : " + String(code));

  String response = http.getString();
  if (response.length() == 0) addToLog("Réponse API vide");
  else addToLog("Réponse API : " + response.substring(0, 400));


  if (code == 200) {
    JsonDocument res; deserializeJson(res, response);
    if (res.containsKey("cmd")) {
      String c = res["cmd"]; int v = res["val"].as<int>();
      addToLog("cmd : " + c + " = " + String(v) );
      if (c == "ch-gazole") orders.force_ch_gazole = v;
      if (c == "ch-gaz")    orders.force_ch_gaz = v;
      if (c == "st-chauffe-eau")    orders.force_ch_eau = v;
      if (c == "tare") {
          gazOrder.pendingTare = true;
          sauverOrdreGaz();
      }
      if (c == "gaz-config") {
          String vStr = res["val"].as<String>(); // On récupère "pvide|pplein"
          int sep = vStr.indexOf('|');
          if (sep > 0) {
            gazOrder.pVide = vStr.substring(0, sep).toFloat();
            gazOrder.pPlein = vStr.substring(sep + 1).toFloat();
            gazOrder.pendingConfig = true;
            sauverOrdreGaz();
            addToLog("Ordre Config Gaz mémorisé : " + vStr);
          }
        }
    }
  }
  
  http.end();
}

void handleStatus() {
  String json = "{";
  int activeChaufP = 0;
  int activeChaufG = 0;
  int forceVentilP = 0;
  if(petroleAutorise()&&digitalRead(pinChauffage) == HIGH ) {
    activeChaufP = 1;
    if(Chauffages.levelChaleur>0) {
      if(digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
      if(digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
      if(digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
    }
  }
  else { 
      if(gazAutorise()&&digitalRead(pinChauffage) == HIGH ) {
      activeChaufG = 1;
      if(Chauffages.levelChaleur>0) {
        if(digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
        if(digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
        if(digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
      }
    } 
  }
  if(petroleAutorise()&&digitalRead(pinChauffeEauInput) == HIGH) {
    activeChaufP = 1;
  }
  if(orders.force_ch_gazole==1) {
    activeChaufP = 1;
  }
  if(orders.force_ch_eau==1) {
    activeChaufP = 1;
  }
  if(orders.force_ch_gaz==1) {
    activeChaufG = 1;
  }


  // --- ÉTAT DU THERMOSTAT (LOCAL) ---
  json += "\"chauffepetrole\": " + String(activeChaufP) + ",";
  json += "\"chauffegaz\": " + String(activeChaufG) + ",";
  json += "\"vitesseventil\": " + String(forceVentilP) + ",";
  
  // --- ÉTAT DES JAUGES ---
  json += "\"jauges\": {";
  json += "\"petrole\": " + String(Jauges.levelPetrole) + ",";
  json += "\"gaz\": " + String(Jauges.levelGaz) + ",";
  json += "\"volt\": " + String(Jauges.levelVolt) + ",";
  json += "\"online\": " + String((millis() - Jauges.lastSeen < 600000) ? "true" : "false");
  json += "},";

  long secP = (millis() - Chauffages.lastSeenPetrole) / 1000;
  bool onlineP = (Chauffages.lastSeenPetrole > 0 && secP < 600);
  long secG = (millis() - Chauffages.lastSeenGaz) / 1000;
  bool onlineG = (Chauffages.lastSeenGaz > 0 && secG < 600);

  json += "\"eauchaud\": " + String(Chauffages.levelChaleur) + ",";
  json += "\"flammegaz\": " + String(Chauffages.levelFlame) + "";

  json += "}";

  // Très important : on précise au client que c'est du JSON
  server.send(200, "application/json", json);
}

// --- HANDLERS API ---
void handleUpdateJaugesGaz() {
  Jauges.levelGaz = server.arg("val").toInt();
  Jauges.lastSeen = millis();
  server.send(200, "text/plain", "OK");
}
void handleUpdateJaugesVolt() {
  Jauges.levelVolt = server.arg("val").toFloat();
  Jauges.lastSeen = millis();
  server.send(200, "text/plain", "OK");
}

void handleUpdateJaugesPetrole() {
  Jauges.levelPetrole = server.arg("val").toInt();
  Jauges.lastSeen = millis();
  server.send(200, "text/plain", "OK");
}


void handleUpdateChauffagePetrole() {
  Chauffages.levelChaleur = server.arg("val").toInt();
  Chauffages.lastSeenPetrole = millis();
  server.send(200, "text/plain", "OK");
}

void handleUpdateChauffageGaz() {
  Chauffages.levelFlame = server.arg("val").toInt();
  Chauffages.lastSeenGaz = millis();
  server.send(200, "text/plain", "OK");
}

String trouverIp(String name) {
  int n = MDNS.queryService("http", "tcp");
  if (n == 0) {
    return ""; // Personne trouvé
  } else {
    for (int i = 0; i < n; ++i) {
      if (MDNS.hostname(i) == name) {
        addToLog(name+" trouvé : "+MDNS.address(i).toString());
        return MDNS.address(i).toString();
      }
    }
  }
  return "";
}

void handleRoot() {
  int activeChaufP = 0;
  int activeChaufG = 0;
  int forceVentilP = 0;
  if(petroleAutorise()&&digitalRead(pinChauffage) == HIGH) {
    activeChaufP = 1;
    if(Chauffages.levelChaleur>0) {
      if(digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
      if(digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
      if(digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
    }
  }
  else { 
      if(gazAutorise()&&digitalRead(pinChauffage) == HIGH) {
      activeChaufG = 1;
      if(Chauffages.levelChaleur>0) {
        if(digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
        if(digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
        if(digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
      }
    } 
  }
  if(petroleAutorise()&&digitalRead(pinChauffeEauInput) == HIGH) {
    activeChaufP = 1;
  }
  if(orders.force_ch_gazole==1) {
    activeChaufP = 1;
  }
  if(orders.force_ch_eau==1) {
    activeChaufP = 1;
  }
  if(orders.force_ch_gaz==1) {
    activeChaufG = 1;
  }
  long sec = (millis() - Jauges.lastSeen) / 1000;
  bool online = (Jauges.lastSeen > 0 && sec < 600);
  long secP = (millis() - Chauffages.lastSeenPetrole) / 1000;
  bool onlineP = (Chauffages.lastSeenPetrole > 0 && secP < 600);
  long secG = (millis() - Chauffages.lastSeenGaz) / 1000;
  bool onlineG = (Chauffages.lastSeenGaz > 0 && secG < 600);

  String html = "<html><head><meta charset='UTF-8' name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#121212; color:#eee; font-family:sans-serif; margin:0; padding-bottom:80px;} ";
  html += ".nav{display:flex; justify-content:space-around; background:#1e1e1e; padding:15px; position:fixed; bottom:0; width:100%; border-top:1px solid #333;} ";
  html += ".nav a{color:#888; text-decoration:none; font-size:0.8em; font-weight:bold;} .page{display:none; padding:20px;} .page.active{display:block;} ";
  html += ".card{background:#1e1e1e; padding:15px; border-radius:10px; margin-bottom:15px; border:1px solid #333; border-left:4px solid #2196f3;} ";
  html += "input, button{display:block; width:100%; margin:10px 0; padding:12px; border-radius:5px; border:none; background:#222; color:white;} ";
  html += "button{background:#2196f3; font-weight:bold;} .status{display:inline-block; width:12px; height:12px; border-radius:50%; margin:0px 8px;} ";
  html += ".on{background:#2ecc71;} .off{background:#e74c3c;}</style></head><body>";

  // PAGE 1 : DASHBOARD (États réels et distants)
  html += "<div id='p-dash' class='page active'><h1>Caravane Dashboard</h1>";

  html += "<div class='card'><b>Chauffage :</b> ";
  if(digitalRead(pinChauffage) == HIGH) {
    html += "ON";
    if(activeChaufP==1) html += " > Petrole > Autorisé";
    else if(activeChaufG==1) html += " > Gaz > Autorisé";
    else html += " > Gaz & Petrole > Refusé";
  }
  else {
    html += "OFF";
  }
  html += "</div>";
  html += "<div class='card'><b>Chauffe eau :</b> ";
  if(digitalRead(pinChauffeEauInput) == HIGH) {
    html += "ON";
  }
  else {
    html += "OFF";
  }
  html += "</div>";
  html += "<div class='card'><b>Vitesse pulseur :</b> ";
  if(digitalRead(pinVit1) == HIGH) html += "1";
  else if(digitalRead(pinVit2) == HIGH) html += "2";
  else if(digitalRead(pinVit3) == HIGH) html += "3";
  else html += "OFF";
  html += "</div>";
  html += "<div class='card'><b>Retour Chaleur Chauffage Petrole :</b><span class='status " + String(onlineP ? "on" : "off") + "'></span> " + String(Chauffages.levelChaleur ? "CHAUD" : "FROID") + "</div>";
  html += "<div class='card'><b>Retour Flame Chauffage GAZ :</b><span class='status " + String(onlineG ? "on" : "off") + "'></span> " + String(Chauffages.levelFlame ? "ALLUME" : "ETEIND") + "</div>";

  html += "<div class='card'><b>Gazole :</b> " + String(Jauges.levelPetrole) + "/4</div>";
  html += "<div class='card'><b>Gaz :</b> " + String(Jauges.levelGaz) + "%</div>";
  html += "<div class='card'><b>Volt :</b> " + String(Jauges.levelVolt) + "V</div>";
  
  html += "</div>";


  // PAGE 2 : MODULES (mDNS dynamique)
  html += "<div id='p-menu' class='page'><h1>Réseau Local</h1>";
  int n = MDNS.queryService("http", "tcp");
  for (int i = 0; i < n; ++i) {
      if (MDNS.hostname(i) != "modul-thermostat") {
        String ip = trouverIp(MDNS.hostname(i));
        if(ip!="") {
          html += "<a href='http://" + ip + "' style='display:block; padding:15px; background:#222; color:#2196f3; text-decoration:none; margin-bottom:10px; border-radius:5px;'>🌐 " + MDNS.hostname(i) + "</a>";
        }
      }
  }
  html += "<button onclick='location.reload()'>SCANNER</button></div>";

  // PAGE 3 : CONFIG
  html += "<div id='p-config' class='page'><h1>Configuration</h1><form action='/save' method='POST' class='card'>";
  html += "SSID: <input name='s' value='"+ext_ssid+"'>PASS: <input name='p' type='password' value='"+ext_pass+"'>API: <input name='a' value='"+api_url+"'>KCO: <input name='k' value='"+kco_key+"'><button type='submit'>SAUVER</button></form></div>";

  html += "<div class='nav'><a href='#' onclick='show(\"p-dash\")'>📊 DASH</a><a href='#' onclick='show(\"p-menu\")'>🔗 MODULES</a><a href='#' onclick='show(\"p-config\")'>⚙️ CONFIG</a></div>";
  html += "<script>function show(id){document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));document.getElementById(id).classList.add('active');}</script></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
    prefs.putString("ssid", server.arg("s")); prefs.putString("pass", server.arg("p"));
    prefs.putString("api", server.arg("a")); prefs.putString("kco", server.arg("k"));
    server.send(200, "text/plain", "OK. Reboot..."); delay(2000); ESP.restart();
}

void addToLog(String msg) {
  String entry = "[" + String(millis() / 1000) + "s] " + msg;
  Serial.println(entry);
  if (telnet.isConnected()) { 
    telnet.println(entry); 
  }
}

void sauverOrdreGaz() {
    prefs.begin(PREF_GAZ, false);
    prefs.putBool("t", gazOrder.pendingTare);
    prefs.putBool("c", gazOrder.pendingConfig);
    prefs.putFloat("v", gazOrder.pVide);
    prefs.putFloat("p", gazOrder.pPlein);
    prefs.end();
}

void chargerOrdreGaz() {
    prefs.begin(PREF_GAZ, true);
    gazOrder.pendingTare = prefs.getBool("t", false);
    gazOrder.pendingConfig = prefs.getBool("c", false);
    gazOrder.pVide = prefs.getFloat("v", 0);
    gazOrder.pPlein = prefs.getFloat("p", 0);
    prefs.end();
}
