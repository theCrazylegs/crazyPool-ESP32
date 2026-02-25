/*
CRAZYPOOL V2
- ESP32
- Sonde PH DFRobot SKU:SEN0161-V2 en 3v3
  -> 3 Boutons Poussoir pour le Calibrage de la sonde PH
- Sonde Temp DFRobot SKU:DFR0198 (DS18B20) sur ESP32 5v
- Ecran LCD Pour Affichage de la Temperature et du PH
- Energy PZEM-004t

REFACTORING V2 — Stabilité + Robustesse
- WiFi + MQTT 100% non-bloquants (millis() state machine)
- Boutons : détection de front montant (edge detection)
- Watchdog timer : reboot automatique si le code se bloque
- ID MQTT unique basé sur l'adresse MAC
- ArduinoJson : construction JSON sans fragmentation mémoire
- MQTT LWT : broker publie "offline" si déconnexion brutale
- Valeurs NaN PZEM remplacées par 0 dans le JSON
*/

#include <DFRobot_ESP_PH.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal.h>
#include <PZEM004Tv30.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "secrets.h"


// ─── Constantes ─────────────────────────────────────────────────────────────

#define WDT_TIMEOUT_S      30    // Watchdog : reboot si loop() bloquée > 30s
#define SENSOR_INTERVAL  10000U  // Lecture capteurs toutes les 10s
#define MQTT_RETRY_INTERVAL 5000U // Tentative MQTT toutes les 5s si déconnecté

// Broches
#define pinBtEnter  4
#define pinBtCal    2
#define pinBtExit   15
#define PH_PIN      33   // fil Bleu
#define DS18B20_Pin 13   // fil Vert

// Résolution ADC de l'ESP32
#define ESPADC     4096.0
#define ESPVOLTAGE 3300

// Topics MQTT
#define MQTT_TOPIC_DATA   "esp32/crazypool"
#define MQTT_TOPIC_STATUS "esp32/crazypool/status"


// ─── Objets ─────────────────────────────────────────────────────────────────

DFRobot_ESP_PH ph;
PZEM004Tv30    pzem(Serial2, 16, 17);  // Serial2, RX2=GPIO16, TX2=GPIO17
LiquidCrystal  lcd(23, 22, 21, 19, 18, 5);
OneWire        ds(DS18B20_Pin);
DallasTemperature sensors(&ds);
WiFiClient     espClient;
PubSubClient   mqttclient(espClient);


// ─── Variables d'état ───────────────────────────────────────────────────────

float phVoltage, phValue, temperature = 25;

// Timers non-bloquants
unsigned long sensorTimer    = 0;
unsigned long mqttRetryTimer = 0;
bool          firstRun       = true;

// Edge detection boutons (détection du front montant uniquement)
bool lastBtEnter = false;
bool lastBtCal   = false;
bool lastBtExit  = false;


// ─── Déclarations forward ───────────────────────────────────────────────────

void readSensorsAndPublish();
void tryMqttConnect();
void handleButtons();
float readTemperature();
void onWifiConnected(WiFiEvent_t event, WiFiEventInfo_t info);
void onWifiGotIP(WiFiEvent_t event, WiFiEventInfo_t info);
void onWifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
void mqttCallback(char* topic, byte* payload, unsigned int length);


// ─── setup() ────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  // PAS de while(!Serial) — bloque le démarrage standalone sans PC

  // Watchdog : reboot automatique si la boucle principale se fige
  esp_task_wdt_init(WDT_TIMEOUT_S, true);  // timeout, panic=true → reboot
  esp_task_wdt_add(NULL);                  // surveille la tâche courante (loop)

  // WiFi — event-driven, reconnexion automatique gérée par le stack
  WiFi.disconnect(true);
  delay(100);
  WiFi.onEvent(onWifiConnected,    ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWifiGotIP,        ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onWifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setAutoReconnect(true);  // reconnexion auto sans intervention du code
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  Serial.println("[WiFi] Connexion en cours...");

  // MQTT — configuration uniquement (pas de connexion bloquante ici)
  mqttclient.setServer(MQTT_BROKER, MQTT_BROKER_PORT);
  mqttclient.setCallback(mqttCallback);
  mqttclient.setBufferSize(512);

  EEPROM.begin(32);  // Stockage calibration pH

  // Ecran LCD
  lcd.begin(16, 2);
  lcd.print("CRAZYPOOL");
  lcd.setCursor(2, 1);
  lcd.print((char)223);  // symbole °
  lcd.setCursor(3, 1);
  lcd.print("C");
  lcd.setCursor(10, 1);
  lcd.print("PH:");

  ph.begin();       // Sonde pH
  sensors.begin();  // Sonde température

  pinMode(pinBtEnter, INPUT);
  pinMode(pinBtCal,   INPUT);
  pinMode(pinBtExit,  INPUT);

  Serial.println("[Setup] Prêt.");
}


// ─── loop() ─────────────────────────────────────────────────────────────────

void loop() {
  esp_task_wdt_reset();  // Nourrit le watchdog — prouve que la boucle tourne

  mqttclient.loop();     // Keepalive MQTT + réception messages — NON-BLOQUANT

  handleButtons();       // Détection boutons sans aucun blocage

  // Reconnexion MQTT non-bloquante si WiFi ok mais MQTT déconnecté
  if (WiFi.isConnected() && !mqttclient.connected()) {
    tryMqttConnect();
  }

  // Lecture capteurs + publication toutes les SENSOR_INTERVAL millisecondes
  if (firstRun || (millis() - sensorTimer >= SENSOR_INTERVAL)) {
    sensorTimer = millis();
    firstRun    = false;
    readSensorsAndPublish();
  }
}


// ─── readSensorsAndPublish() ─────────────────────────────────────────────────

void readSensorsAndPublish() {
  // Lecture PZEM (peut renvoyer NaN si non alimenté — géré ci-dessous)
  float voltage   = pzem.voltage();
  float current   = pzem.current();
  float power     = pzem.power();
  float energy    = pzem.energy();
  float frequency = pzem.frequency();
  float pf        = pzem.pf();

  // Lecture température + pH
  temperature = readTemperature();
  phVoltage   = analogRead(PH_PIN) / ESPADC * ESPVOLTAGE;
  phValue     = ph.readPH(phVoltage, temperature);

  // Mise à jour LCD
  lcd.setCursor(0, 0);
  lcd.print("CRAZYPOOL       ");
  lcd.setCursor(0, 1);
  lcd.print(temperature, 0);
  lcd.setCursor(13, 1);
  lcd.print(phValue, 1);

  // Publication MQTT uniquement si les deux connexions sont actives
  if (!WiFi.isConnected() || !mqttclient.connected()) {
    Serial.printf("[Publish] Skipped — WiFi:%d MQTT:%d\n",
                  WiFi.isConnected(), mqttclient.connected());
    return;
  }

  // Construction JSON avec ArduinoJson — allocation propre, pas de fragmentation
  // Les valeurs NaN du PZEM sont remplacées par 0 plutôt qu'envoyées telles quelles
  JsonDocument doc;
  doc["temperature"]  = temperature;
  doc["ph"]           = phValue;
  doc["Volt"]         = isnan(voltage)   ? 0.0f : voltage;
  doc["Ampere"]       = isnan(current)   ? 0.0f : current;
  doc["Watts"]        = isnan(power)     ? 0.0f : power;
  doc["Kwh"]          = isnan(energy)    ? 0.0f : energy;
  doc["Hz"]           = isnan(frequency) ? 0.0f : frequency;
  doc["Power_factor"] = isnan(pf)        ? 0.0f : pf;

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

  Serial.println(jsonBuffer);
  mqttclient.publish(MQTT_TOPIC_DATA, jsonBuffer, true);
}


// ─── tryMqttConnect() ────────────────────────────────────────────────────────
// Tente une connexion MQTT UNE SEULE FOIS et rend la main immédiatement.
// Rappelée depuis loop() toutes les MQTT_RETRY_INTERVAL ms si déconnecté.

void tryMqttConnect() {
  if (millis() - mqttRetryTimer < MQTT_RETRY_INTERVAL) return;
  mqttRetryTimer = millis();

  // ID unique basé sur l'adresse MAC — évite les conflits si plusieurs ESP32
  String clientId = "CrazyPool-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] Connexion id=%s ...\n", clientId.c_str());

  // LWT : si l'ESP32 perd le courant ou plante, le broker publie "offline" tout seul
  if (mqttclient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_KEY,
                         MQTT_TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("[MQTT] Connecte !");
    lcd.setCursor(12, 0);
    lcd.print("MQTT");
    // Annoncer qu'on est en ligne (retained → Home Assistant le voit même après reconnexion)
    mqttclient.publish(MQTT_TOPIC_STATUS, "online", true);
  } else {
    Serial.printf("[MQTT] Echec code=%d, retry dans %lus\n",
                  mqttclient.state(), MQTT_RETRY_INTERVAL / 1000);
    lcd.setCursor(12, 0);
    lcd.print("MERR");
  }
  // On retourne immédiatement — AUCUN délai, AUCUNE boucle
}


// ─── handleButtons() ─────────────────────────────────────────────────────────
// Edge detection : réagit uniquement quand le bouton PASSE de relâché à pressé.
// Aucun while(), aucun delay() — la boucle principale continue de tourner.

void handleButtons() {
  bool curEnter = digitalRead(pinBtEnter);
  bool curCal   = digitalRead(pinBtCal);
  bool curExit  = digitalRead(pinBtExit);

  if (curEnter && !lastBtEnter) {  // Front montant ENTER
    ph.calibration(phVoltage, temperature, (char*)"ENTERPH");
    lcd.setCursor(9, 1);
    lcd.print("CAL:");
  }

  if (curCal && !lastBtCal) {      // Front montant CAL
    ph.calibration(phVoltage, temperature, (char*)"CALPH");
    lcd.setCursor(12, 0);
    lcd.print("save");
  }

  if (curExit && !lastBtExit) {    // Front montant EXIT
    ph.calibration(phVoltage, temperature, (char*)"EXITPH");
    lcd.setCursor(12, 0);
    lcd.print("    ");
    lcd.setCursor(9, 1);
    lcd.print(" PH:");
  }

  // Mémoriser l'état pour la prochaine itération
  lastBtEnter = curEnter;
  lastBtCal   = curCal;
  lastBtExit  = curExit;
}


// ─── readTemperature() ───────────────────────────────────────────────────────

float readTemperature() {
  sensors.requestTemperatures();
  return sensors.getTempCByIndex(0);
}


// ─── Events WiFi ─────────────────────────────────────────────────────────────

void onWifiConnected(arduino_event_id_t event, arduino_event_info_t info) {
  Serial.println("[WiFi] Associé au point d'accès");
}

void onWifiGotIP(arduino_event_id_t event, arduino_event_info_t info) {
  Serial.print("[WiFi] IP : ");
  Serial.println(WiFi.localIP());
  lcd.setCursor(12, 0);
  lcd.print("WIFI");
  mqttRetryTimer = 0;  // Déclencher une tentative MQTT immédiate
}

void onWifiDisconnected(arduino_event_id_t event, arduino_event_info_t info) {
  Serial.printf("[WiFi] Deconnecte, raison : %d\n", info.wifi_sta_disconnected.reason);
  lcd.setCursor(12, 0);
  lcd.print("NWIF");
  // WiFi.setAutoReconnect(true) gere la reconnexion — rien a faire ici
}


// ─── Callback MQTT ───────────────────────────────────────────────────────────

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message reçu — topic : %s\n", topic);
  // Traitement des commandes entrantes à implémenter ici si besoin
}
