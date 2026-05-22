#include <WiFiClientSecure.h>
#include "WiFi.h"
#include "Preferences.h"
#include "WebServer.h"
#include "ESPTelnet.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"

WebServer server(80);
Preferences prefs;
ESPTelnet telnet;

// Pins entrées
const int pinChauffage = 25;
const int pinVit1 = 35;
const int pinVit2 = 32;
const int pinVit3 = 33;
const int pinChauffeEauInput = 26; // L'interrupteur d'activation du chauffe eau
const int pinLedEauChaude = 27;    // Le négatif de la LED

String ext_ssid, ext_pass, api_url, kco_key, ota_pass;
int canal;
unsigned long lastApiSync = 0;

#define MODE_GAZOLE 0
#define MODE_GAZ    1
#define MODE_ELEC   2

struct ChauffageOrder {
  int mode = 0; // 0 à 5 : liste des 6 ordres possibles
} chOrder;

struct RemoteOrders {
  int force_ch_gazole = -1;
  int force_ch_gaz    = -1;
  int force_ch_elec   = -1;
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
  int levelTemperature;
  int levelFlame;
};

RemoteJauge Jauges = {0, 0, 0, "JAUGES RÉSERVOIR", 0};
MacChauffage Chauffages = {0, 0, "Chauffage au pétrole", "Chauffage au gaz", 0, 0};

// --- GESTION DES ORDRES GAZ ---
struct GazOrder {
  bool pendingTare = false;
  float pVide = 0;
  float pPlein = 0;
  bool pendingConfig = false;
} gazOrder;

// Nom pour Preferences
const char* PREF_GAZ = "gaz_mem";




int getOrderValue(int mode, int index) {
  const int orders[6][3] = {
    {MODE_GAZOLE, MODE_GAZ,    MODE_ELEC},   // 0 Gazole > Gaz > Electrique
    {MODE_GAZOLE, MODE_ELEC,   MODE_GAZ},    // 1 Gazole > Electrique > Gaz
    {MODE_GAZ,    MODE_ELEC,   MODE_GAZOLE}, // 2 Gaz > Electrique > Gazole
    {MODE_GAZ,    MODE_GAZOLE, MODE_ELEC},   // 3 Gaz > Gazole > Electrique
    {MODE_ELEC,   MODE_GAZ,    MODE_GAZOLE}, // 4 Electrique > Gaz > Gazole
    {MODE_ELEC,   MODE_GAZOLE, MODE_GAZ}     // 5 Electrique > Gazole > Gaz
  };

  if (mode < 0 || mode > 5) mode = 0;
  if (index < 0 || index > 2) index = 0;
  return orders[mode][index];
}

String orderLabel(int mode) {
  if (mode == 0) return "Gazole > Gaz > Electrique";
  if (mode == 1) return "Gazole > Electrique > Gaz";
  if (mode == 2) return "Gaz > Electrique > Gazole";
  if (mode == 3) return "Gaz > Gazole > Electrique";
  if (mode == 4) return "Electrique > Gaz > Gazole";
  if (mode == 5) return "Electrique > Gazole > Gaz";
  return "Gazole > Gaz > Electrique";
}

void saveChauffageOrder() {
  Preferences p;
  p.begin("ch_order", false);
  p.putInt("mode", chOrder.mode);
  p.end();
}

void loadChauffageOrder() {
  Preferences p;
  p.begin("ch_order", true);
  chOrder.mode = p.getInt("mode", 0);
  p.end();

  if (chOrder.mode < 0 || chOrder.mode > 5) {
    chOrder.mode = 0;
    saveChauffageOrder();
  }
}

int choisirChauffage() {
  for (int i = 0; i < 3; i++) {
    int mode = getOrderValue(chOrder.mode, i);

    if (mode == MODE_GAZOLE && petroleAutorise()) return MODE_GAZOLE;
    if (mode == MODE_GAZ && gazAutorise()) return MODE_GAZ;
    if (mode == MODE_ELEC) return MODE_ELEC;
  }
  return MODE_ELEC;
}

void setup() {
  Serial.begin(115200);

  // Config des entrées
  pinMode(pinChauffage, INPUT_PULLDOWN);
  pinMode(pinVit1, INPUT_PULLDOWN);
  pinMode(pinVit2, INPUT_PULLDOWN);
  pinMode(pinVit3, INPUT_PULLDOWN);
  pinMode(pinChauffeEauInput, INPUT_PULLDOWN);
  pinMode(pinLedEauChaude, OUTPUT);
  digitalWrite(pinLedEauChaude, HIGH);

  prefs.begin("config", false);
  ext_ssid = prefs.getString("ssid", "");
  ext_pass = prefs.getString("pass", "");
  canal = prefs.getInt("canal", 1);
  api_url  = prefs.getString("api", "https://carsadventures.fr/wp-json/api/v1/esp32/");
  kco_key  = prefs.getString("kco", "");
  ota_pass = "dyqpa8we";

  loadChauffageOrder();
  chargerOrdreGaz();

  // 1. WIFI CACHÉ pour l'interconnexion
  WiFi.softAP("CarsNet_Internal", "AdminCars123", canal, 1);

  // 2. WIFI EXTÉRIEUR
  if (ext_ssid != "") WiFi.begin(ext_ssid.c_str(), ext_pass.c_str());

  telnet.begin();
  telnet.onConnect([](String ip) {
    addToLog("\nBienvenue sur le Modul-Thermostat !");
  });

  // 3. MDNS
  if (MDNS.begin("modul-thermostat")) {
    MDNS.addService("http", "tcp", 80);
  }

  // 4. OTA Android / ArduinoOTA avec password
  ArduinoOTA.setHostname("modul-thermostat");
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

  server.on("/", handleDash);
  server.on("/modules", handleModules);
  server.on("/config", handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/updatepetrole", handleUpdateJaugesPetrole);
  server.on("/updategaz", handleUpdateJaugesGaz);
  server.on("/updatevolt", handleUpdateJaugesVolt);
  server.on("/updatechauffagegaz", handleUpdateChauffageGaz);
  server.on("/updatechauffagepetroletemperature", handleUpdateChauffagePetroleTemperature);
  server.on("/status", handleStatus);

  server.on("/setOrder", HTTP_POST, []() {
    chOrder.mode = server.arg("mode").toInt();
    if (chOrder.mode < 0 || chOrder.mode > 5) chOrder.mode = 0;
    saveChauffageOrder();
    server.send(200, "text/plain", "OK. Ordre chauffage sauvé.");
  });

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
    sauverOrdreGaz();
    server.send(200, "text/plain", "OK");
    addToLog("Ordre Gaz purgé (Jauge a récupéré)");
  });

  server.begin();
  addToLog("\nWiFi OK : " + WiFi.localIP().toString());
  addToLog("OTA actif hostname modul-thermostat avec password configuré");
}

void loop() {
  server.handleClient();
  telnet.loop();
  ArduinoOTA.handle();

  // 1. Lire si l'utilisateur veut le chauffe-eau
  bool veutChauffeEau = (digitalRead(pinChauffeEauInput) == HIGH);

  // 2. Vérifier si l'eau est chaude (Info venant du module distant Pétrole)
  long secContact = (millis() - Chauffages.lastSeenPetrole) / 1000;
  bool eauEstChaude = (Chauffages.levelTemperature >= 60 && secContact < 600);

  // 3. Piloter la LED
  if (eauEstChaude) {
    digitalWrite(pinLedEauChaude, LOW);
  } else {
    digitalWrite(pinLedEauChaude, HIGH);
  }

  if (millis() - lastApiSync > 10000) {
    syncWithAPI();
    lastApiSync = millis();
  }
}

bool petroleAutorise() {
  long sec = (millis() - Jauges.lastSeen) / 1000;
  bool online = (Jauges.lastSeen > 0 && sec < 600);
  if (online && Jauges.levelPetrole > 1 && Jauges.levelPetrole != 0 && Jauges.levelVolt > 12.1) { return true; }
  if (!online) { return true; }
  return false;
}

bool gazAutorise() {
  long sec = (millis() - Jauges.lastSeen) / 1000;
  bool online = (Jauges.lastSeen > 0 && sec < 600);
  if (online && Jauges.levelGaz > 1 && Jauges.levelGaz != 0) { return true; }
  if (!online) { return true; }
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
  int activeChaufL = 0;
  int activeChaufG = 0;
  int forceVentilP = 0;

  if (digitalRead(pinChauffage) == HIGH) {
    int choix = choisirChauffage();

    if (choix == MODE_GAZOLE) {
      activeChaufP = 1;
      if (Chauffages.levelTemperature > 60) {
        if (digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
        if (digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
        if (digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
      }
    } else if (choix == MODE_GAZ) {
      activeChaufG = 1;
    } else {
      activeChaufL = 1;
    }
  }

  if (petroleAutorise() && digitalRead(pinChauffeEauInput) == HIGH) {
    activeChaufE = 1;
  }

  if (orders.force_ch_gazole == 1) {
    addToLog("force gazole detecté");
    activeChaufP = 1;
    if (Chauffages.levelTemperature > 60) {
      if (digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
      if (digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
      if (digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
    }
  }

  if (orders.force_ch_eau == 1) {
    addToLog("force eau detecté");
    activeChaufE = 1;
  }

  if (orders.force_ch_gaz == 1) {
    addToLog("force gaz detecté");
    activeChaufG = 1;
  }

  if (orders.force_ch_elec == 1) {
    addToLog("force elec detecté");
    activeChaufL = 1;
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
  addLogJson("st-ch-elec", String(activeChaufL));
  addLogJson("st-chauffe-eau", String(activeChaufE));
  addLogJson("st-temperature", String(Chauffages.levelTemperature));
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
  client.setInsecure();

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
    JsonDocument res;
    deserializeJson(res, response);

    if (res.containsKey("cmd")) {
      String c = res["cmd"];
      int v = res["val"].as<int>();
      addToLog("cmd : " + c + " = " + String(v));

      if (c == "ch-gazole") orders.force_ch_gazole = v;
      if (c == "ch-gaz")    orders.force_ch_gaz = v;
      if (c == "ch-elec")   orders.force_ch_elec = v;
      if (c == "st-chauffe-eau") orders.force_ch_eau = v;
      if( c == "change-order") {
        chOrder.mode = v;
        if (chOrder.mode < 0 || chOrder.mode > 5) chOrder.mode = 0;
        saveChauffageOrder();
      }
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
  int activeChaufL = 0;
  int forceVentilP = 0;

  if (digitalRead(pinChauffage) == HIGH) {
    int choix = choisirChauffage();

    if (choix == MODE_GAZOLE) {
      activeChaufP = 1;
      if (Chauffages.levelTemperature > 60) {
        if (digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
        if (digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
        if (digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
      }
    } else if (choix == MODE_GAZ) {
      activeChaufG = 1;
    } else {
      activeChaufL = 1;
    }
  }

  if (petroleAutorise() && digitalRead(pinChauffeEauInput) == HIGH) {
    activeChaufP = 1;
  } else if (digitalRead(pinChauffeEauInput) == HIGH) {
    activeChaufL = 1;
  }

  if (orders.force_ch_gazole == 1) {
    activeChaufP = 1;
    if (Chauffages.levelTemperature > 60) {
      if (digitalRead(pinVit1) == HIGH) { forceVentilP = 1; }
      if (digitalRead(pinVit2) == HIGH) { forceVentilP = 2; }
      if (digitalRead(pinVit3) == HIGH) { forceVentilP = 3; }
    }
  }

  if (orders.force_ch_eau == 1) {
    activeChaufP = 1;
  }

  if (orders.force_ch_gaz == 1) {
    activeChaufG = 1;
  }

  if (orders.force_ch_elec == 1) {
    activeChaufL = 1;
  }

  json += "\"chauffepetrole\": " + String(activeChaufP) + ",";
  json += "\"chauffegaz\": " + String(activeChaufG) + ",";
  json += "\"chauffeelec\": " + String(activeChaufL) + ",";
  json += "\"vitesseventil\": " + String(forceVentilP) + ",";

  json += "\"ordre_chauffage_mode\": " + String(chOrder.mode) + ",";

  json += "\"jauges\": {";
  json += "\"petrole\": " + String(Jauges.levelPetrole) + ",";
  json += "\"gaz\": " + String(Jauges.levelGaz) + ",";
  json += "\"volt\": " + String(Jauges.levelVolt) + ",";
  json += "\"online\": " + String((millis() - Jauges.lastSeen < 600000) ? "true" : "false");
  json += "},";

  json += "\"eauchaud\": " + String(Chauffages.levelTemperature) + ",";
  json += "\"flammegaz\": " + String(Chauffages.levelFlame);

  json += "}";

  server.send(200, "application/json", json);
}

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

void handleUpdateChauffagePetroleTemperature() {
  Chauffages.levelTemperature = server.arg("val").toInt();
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
    return "";
  } else {
    for (int i = 0; i < n; ++i) {
      if (MDNS.hostname(i) == name) {
        addToLog(name + " trouvé : " + MDNS.address(i).toString());
        return MDNS.address(i).toString();
      }
    }
  }
  return "";
}

String htmlHeader(String title) {
  String html;
  html.reserve(6000);

  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>" + title + "</title>";

  html += "<style> body{background:#121212;color:#eee;font-family:sans-serif;margin:0;padding:20px 20px 90px 20px;} .card{background:#1e1e1e;padding:15px;border-radius:10px;margin-bottom:15px;border:1px solid #333;border-left:4px solid #2196f3;} input,select,button{display:block;width:100%;box-sizing:border-box;margin:10px 0;padding:12px;border-radius:5px;border:none;background:#222;color:white;} button{background:#2196f3;font-weight:bold;} .status{display:inline-block;width:12px;height:12px;border-radius:50%;margin:0 8px;} .on{background:#2ecc71;}.off{background:#e74c3c;} .nav{display:flex;justify-content:space-around;background:#1e1e1e;padding:15px;position:fixed;bottom:0;left:0;width:100%;border-top:1px solid #333;} .nav a{color:#aaa;text-decoration:none;font-size:.85em;font-weight:bold;} a{color:#2ecc71;text-decoration:none;font-weight:bold;} </style>";

  html += "</head><body><h1>" + title + "</h1>";

  return html;
}
String htmlFooter() {
  String html = "<div class='nav'><a href='/'>📊 DASH</a><a href='/modules'>🔗 MODULES</a><a href='/config'>⚙️ CONFIG</a></div></body></html>";
  return html;
}
void handleDash() {
auto isOnline = [](unsigned long lastSeen) { 
    return (lastSeen > 0 && (millis() - lastSeen) / 1000 < 600); 
  };
  bool onlineP = isOnline(Chauffages.lastSeenPetrole);
  bool onlineG = isOnline(Chauffages.lastSeenGaz);

  String html = htmlHeader("Caravane Dashboard");

  // 2. Section Chauffage (Logique simplifiée)
  html += "<div class='card'><b>Chauffage :</b> ";
  if (digitalRead(pinChauffage) == HIGH) {
    html += "ON";
    int choix = choisirChauffage();
    // Affichage mode automatique / autorisé
    if (choix == MODE_GAZOLE) html += " > Petrole > Autorisé";
    else if (choix == MODE_GAZ) html += " > Gaz > Autorisé";
    else html += " > Electrique";

    // Affichage Forçage (si applicable)
    if (orders.force_ch_gazole)      html += " | > Petrole > Forcé";
    else if (orders.force_ch_gaz)    html += " | > Gaz > Forcé";
    else if (orders.force_ch_elec)   html += " | > Electrique > Forcé";
  } else {
    html += "OFF";
  }
  html += "</div>";

  // 3. Section Ordre (Utilisation d'un tableau pour éviter les if/else)
  const char* modes[] = {
    "Gazole > Gaz > Electrique", "Gazole > Electrique > Gaz",
    "Gaz > Electrique > Gazole", "Gaz > Gazole > Electrique",
    "Electrique > Gaz > Gazole", "Electrique > Gazole > Gaz"
  };
  html += "<div class='card'><b>Ordre préférence :</b> <b>Priorité actuelle :</b> ";
  html += (chOrder.mode >= 0 && chOrder.mode <= 5) ? modes[chOrder.mode] : modes[0];
  html += "</div>";

  // 4. Capteurs et Status (Logique ternaire directe)
  html += "<div class='card'><b>Chauffe eau :</b> " + String(digitalRead(pinChauffeEauInput) == HIGH ? "ON" : "OFF") + "</div><div class='card'><b>Vitesse pulseur :</b> " +  String(digitalRead(pinVit3) == HIGH ? "3" : (digitalRead(pinVit2) == HIGH ? "2" : (digitalRead(pinVit1) == HIGH ? "1" : "OFF"))) + "</div><div class='card'><b>Retour Chaleur Petrole :</b><span class='status " + String(onlineP ? "on" : "off") + "'></span> Température : "+String(Chauffages.levelTemperature)+"°C</div><div class='card'><b>Retour Flame GAZ :</b><span class='status " + String(onlineG ? "on" : "off") + "'></span> " + (Chauffages.levelFlame ? "ALLUME" : "ETEIND") + "</div><div class='card'><b>Gazole :</b> " + String(Jauges.levelPetrole) + "/4</div><div class='card'><b>Gaz :</b> " + String(Jauges.levelGaz) + "%</div><div class='card'><b>Volt :</b> " + String(Jauges.levelVolt) + "V</div>";

  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleModules() {
  String html = htmlHeader("Réseau Local");


  wifi_sta_list_t wifi_sta_list;
  esp_wifi_ap_get_sta_list(&wifi_sta_list);

  if (wifi_sta_list.num == 0) {
      html += "<p style='color:orange;'>Aucun appareil détecté sur le réseau interne.</p>";
  } else {
      html += "<p style='color:orange;'>Nombre d'appareils détectés : "
            + String(wifi_sta_list.num) + "</p>";

      for (int i = 0; i < wifi_sta_list.num; i++) {

          uint8_t* mac = wifi_sta_list.sta[i].mac;

          char macStr[18];
          sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2],
                  mac[3], mac[4], mac[5]);

          String ipStr = "IP inconnue";

          for (int ipLast = 2; ipLast < 255; ipLast++) {
              IPAddress testIp(192, 168, 4, ipLast);

              ip4_addr_t ipaddr;
              IP4_ADDR(&ipaddr, 192, 168, 4, ipLast);

              struct eth_addr* eth_ret = nullptr;
              const ip4_addr_t* ip_ret = nullptr;

              if (etharp_find_addr(nullptr, &ipaddr, &eth_ret, &ip_ret) >= 0 && eth_ret != nullptr) {
                  if (memcmp(eth_ret->addr, mac, 6) == 0) {
                      ipStr = testIp.toString();
                      break;
                  }
              }
          }


          html += "<div class='card' style='border-left:4px solid #2196f3;'>";
          html += "<b>Appareil " + String(i + 1) + "</b><br>";
          html += "<small>IP réelle : " + ipStr + "</small><br>";
          html += "<small>MAC : " + String(macStr) + "</small><br>";
          if(String(macStr) == "34:5F:45:4E:3E:E8") html += "<small>Chauffage Electrique</small><br>";
          if(String(macStr) == "40:F5:20:33:53:DC") html += "<small>Chauffage Gaz</small><br>";
          if(String(macStr) == "1C:C3:AB:BF:A5:AC") html += "<small>Chauffage Pétrole</small><br>";
          if(String(macStr) == "88:57:21:95:54:58") html += "<small>Jauges</small><br>";
          

          if (ipStr != "IP inconnue") {
              html += "<a href='http://" + ipStr + "' style='color:#2ecc71; text-decoration:none; font-weight:bold;'>[OUVRIR L'INTERFACE]</a>";
          }

          html += "</div>";
      }
  }
  html += "<button onclick='location.reload()'>SCANNER À NOUVEAU</button>";
  html += htmlFooter();

  server.send(200, "text/html", html);
}

void handleConfig() {
  String html = htmlHeader("Configuration");

  html += "<form action='/save' method='POST' class='card'>";
  html += "SSID: <input name='s' value='" + ext_ssid + "'>";
  html += "PASS: <input name='p' type='password' value='" + ext_pass + "'>";
  html += "Canal: <input name='c' type='numeric' value='" + String(canal) + "'>";
  html += "API: <input name='a' value='" + api_url + "'>";
  html += "KCO: <input name='k' value='" + kco_key + "'>";
  html += "<button type='submit'>SAUVER</button>";
  html += "</form>";

  html += "<form action='/setOrder' method='POST' class='card'>";
  html += "<h3>Ordre de préférence chauffage</h3>";
  html += "<select name='mode'>";
  html += "<option value='0' " + String(chOrder.mode == 0 ? "selected" : "") + ">Gazole &gt; Gaz &gt; Electrique</option>";
  html += "<option value='1' " + String(chOrder.mode == 1 ? "selected" : "") + ">Gazole &gt; Electrique &gt; Gaz</option>";
  html += "<option value='2' " + String(chOrder.mode == 2 ? "selected" : "") + ">Gaz &gt; Electrique &gt; Gazole</option>";
  html += "<option value='3' " + String(chOrder.mode == 3 ? "selected" : "") + ">Gaz &gt; Gazole &gt; Electrique</option>";
  html += "<option value='4' " + String(chOrder.mode == 4 ? "selected" : "") + ">Electrique &gt; Gaz &gt; Gazole</option>";
  html += "<option value='5' " + String(chOrder.mode == 5 ? "selected" : "") + ">Electrique &gt; Gazole &gt; Gaz</option>";
  html += "</select>";
  html += "<small>Ordre actuel : " + orderLabel(chOrder.mode) + "</small>";
  html += "<button type='submit'>SAUVER ORDRE</button>";
  html += "</form>";

  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleSave() {
  prefs.putString("ssid", server.arg("s"));
  prefs.putString("pass", server.arg("p"));
  prefs.putInt("canal", server.arg("c").toInt());
  prefs.putString("api", server.arg("a"));
  prefs.putString("kco", server.arg("k"));
  prefs.putString("ota", server.arg("ota"));

  server.send(200, "text/plain", "OK. Reboot...");
  delay(2000);
  ESP.restart();
}

void addToLog(String msg) {
  String entry = "[" + String(millis() / 1000) + "s] " + msg;
  Serial.println(entry);
  if (telnet.isConnected()) {
    telnet.println(entry);
  }
}

void sauverOrdreGaz() {
  Preferences p;
  p.begin(PREF_GAZ, false);
  p.putBool("t", gazOrder.pendingTare);
  p.putBool("c", gazOrder.pendingConfig);
  p.putFloat("v", gazOrder.pVide);
  p.putFloat("p", gazOrder.pPlein);
  p.end();
}

void chargerOrdreGaz() {
  Preferences p;
  p.begin(PREF_GAZ, true);
  gazOrder.pendingTare = p.getBool("t", false);
  gazOrder.pendingConfig = p.getBool("c", false);
  gazOrder.pVide = p.getFloat("v", 0);
  gazOrder.pPlein = p.getFloat("p", 0);
  p.end();
}
