/*
 * ═══════════════════════════════════════════════════════════════════════════
 *  CRAZYPOOL V2 — Gestion piscine ESP32
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Capteurs :
 *    - Sonde pH     DFRobot SEN0161-V2 (3.3V)
 *    - Sonde Temp   DFRobot DFR0198 / DS18B20 (5V)
 *    - Énergie      PZEM-004t v3
 *
 *  Affichage :
 *    - LCD 16×2 HD44780
 *
 *  Connectivité :
 *    - WiFi + MQTT (non-bloquants)
 *    - OTA (mise à jour sans câble)
 *
 *  Calibration pH — 1 bouton unique (GPIO 2, pull-down externe) :
 *    - Appui long  3s en IDLE → entre en mode calibration
 *    - Appui court    en CAL  → sauvegarde un point (pH4 ou pH7)
 *    - Appui long  3s en CAL  → quitte et sauvegarde en EEPROM
 *    - Timeout 60s en CAL     → auto-exit + sauvegarde
 *    - Appui court en IDLE    → ignoré (anti-accidentel)
 *
 *  Améliorations V2 vs V1 :
 *    - WiFi + MQTT 100% non-bloquants (millis)
 *    - Watchdog timer (reboot auto si freeze)
 *    - ArduinoJson (JSON propre, pas de fragmentation)
 *    - MQTT LWT ("offline" publié par le broker si crash)
 *    - ID MQTT unique (MAC)
 *    - Température non-bloquante (requestTemperatures async)
 *    - LCD : séparation stricte des zones d'écriture (pas de conflit)
 *    - Log levels (LOG_INFO / LOG_WARN / LOG_ERROR / LOG_DEBUG)
 *
 *  Dépendances (platformio.ini ou Arduino IDE) :
 *    DFRobot_ESP_PH, OneWire, DallasTemperature, LiquidCrystal,
 *    PZEM004Tv30, PubSubClient, ArduinoJson, ArduinoOTA
 *
 * ═══════════════════════════════════════════════════════════════════════════
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
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include "secrets.h"


// ─── Configuration ──────────────────────────────────────────────────────────

// Timings
#define WDT_TIMEOUT_S        30      // Watchdog : reboot si loop() bloquée > 30s
#define SENSOR_INTERVAL      10000U  // Lecture capteurs toutes les 10s
#define MQTT_RETRY_INTERVAL  5000U   // Tentative MQTT toutes les 5s
#define DEBOUNCE_MS          50U     // Anti-rebond bouton (50ms — plus fiable que 20ms)
#define LONGPRESS_MS         3000U   // Durée appui long pour calibration
#define CAL_TIMEOUT_MS       60000U  // Auto-exit calibration si inactif 60s

// Broches
#define PIN_BUTTON    2    // Bouton unique calibration pH (pull-down externe, HIGH=pressé)
#define PH_PIN       33    // Sonde pH — fil bleu
#define DS18B20_PIN  13    // Sonde température — fil vert
// LCD : RS=23, EN=22, D4=21, D5=19, D6=18, D7=5
// PZEM : Serial2 RX=GPIO16, TX=GPIO17

// ADC ESP32
#define ESPADC      4096.0
#define ESPVOLTAGE  3300

// MQTT Topics
#define MQTT_TOPIC_DATA    "esp32/crazypool"
#define MQTT_TOPIC_STATUS  "esp32/crazypool/status"

// Log levels : 0=off  1=error  2=info  3=debug
// Surcharge possible : build_flags = -DLOG_LEVEL=3
#ifndef LOG_LEVEL
  #define LOG_LEVEL 2
#endif

#if LOG_LEVEL >= 3
  #define LOG_DEBUG(fmt, ...) Serial.printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_DEBUG(fmt, ...)
#endif
#if LOG_LEVEL >= 2
  #define LOG_INFO(fmt, ...)  Serial.printf("[INFO]  " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_INFO(fmt, ...)
#endif
#if LOG_LEVEL >= 1
  #define LOG_WARN(fmt, ...)  Serial.printf("[WARN]  " fmt "\n", ##__VA_ARGS__)
  #define LOG_ERROR(fmt, ...) Serial.printf("[ERROR] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_WARN(fmt, ...)
  #define LOG_ERROR(fmt, ...)
#endif


// ─── Objets globaux ─────────────────────────────────────────────────────────

DFRobot_ESP_PH    ph;
PZEM004Tv30       pzem(Serial2, 16, 17);
LiquidCrystal     lcd(23, 22, 21, 19, 18, 5);
OneWire           ds(DS18B20_PIN);
DallasTemperature sensors(&ds);
WiFiClient        espClient;
PubSubClient      mqttclient(espClient);


// ─── Variables d'état ───────────────────────────────────────────────────────

// Capteurs
float phVoltage  = 0;
float phValue    = 7.0;
float temperature = 25.0;

// Timers non-bloquants
unsigned long sensorTimer      = 0;
unsigned long mqttRetryTimer   = 0;
unsigned long tempRequestTimer = 0;
bool          firstRun         = true;
bool          tempPending      = false;

// Machine à états — bouton unique
enum CalState : uint8_t { CAL_IDLE, CAL_ACTIVE };
CalState      calState          = CAL_IDLE;
bool          btnPrev           = false;   // état bouton au cycle précédent
unsigned long btnPressStart     = 0;       // millis() au début de l'appui
bool          longPressFired    = false;   // empêche la répétition du long press
unsigned long calActivityTimer  = 0;       // timer pour auto-exit 60s
bool          calPointSaved     = false;   // un point a été sauvegardé dans cette session
int8_t        lastDotsDisplayed = -1;      // anti-flickering barre de progression
unsigned long calDisplayTimer   = 0;       // rafraîchissement pH en mode CAL (toutes les 2s)

// LCD — flag pour forcer un rafraîchissement propre après changement d'état
bool          lcdNeedsFullRefresh = true;


// ─── Prototypes ─────────────────────────────────────────────────────────────

void handleButton();
void readSensorsAndPublish();
void tryMqttConnect();
void updateLCD();
float readTemperature();

void onWifiConnected(arduino_event_id_t event, arduino_event_info_t info);
void onWifiGotIP(arduino_event_id_t event, arduino_event_info_t info);
void onWifiDisconnected(arduino_event_id_t event, arduino_event_info_t info);
void mqttCallback(char* topic, byte* payload, unsigned int length);


// ═══════════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  // Pas de while(!Serial) — bloquerait le démarrage sans PC connecté

  // ── Watchdog ──
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  // ── WiFi (event-driven, non-bloquant) ──
  WiFi.disconnect(true);
  delay(100);
  WiFi.onEvent(onWifiConnected,    ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(onWifiGotIP,        ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(onWifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  LOG_INFO("WiFi: connexion en cours...");

  // ── MQTT (config seulement, connexion dans loop) ──
  mqttclient.setServer(MQTT_BROKER, MQTT_BROKER_PORT);
  mqttclient.setCallback(mqttCallback);
  mqttclient.setBufferSize(512);

  // ── OTA ──
  #ifndef OTA_PASSWORD
    #warning "OTA_PASSWORD non défini dans secrets.h — mot de passe par défaut utilisé"
    #define OTA_PASSWORD "crazypool-ota"
  #endif
  ArduinoOTA.setHostname("CrazyPool");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    LOG_INFO("OTA: demarrage...");
    esp_task_wdt_reset();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    esp_task_wdt_reset();
  });
  ArduinoOTA.onEnd([]() { LOG_INFO("OTA: termine, redemarrage..."); });
  ArduinoOTA.onError([](ota_error_t error) { LOG_ERROR("OTA: erreur %u", error); });
  ArduinoOTA.begin();

  // ── EEPROM (calibration pH) ──
  EEPROM.begin(32);

  // ── LCD ──
  delay(300);  // le HD44780 a besoin de temps après un reset
  lcd.begin(16, 2);
  lcd.clear();
  lcd.print("CRAZYPOOL  v2");
  lcd.setCursor(0, 1);
  lcd.print("Demarrage...");

  // ── Capteurs ──
  ph.begin();
  sensors.begin();
  sensors.setWaitForConversion(false);  // conversion async (~750ms)

  // ── Bouton ──
  pinMode(PIN_BUTTON, INPUT);  // pull-down externe : HIGH = pressé

  delay(1500);  // laisser le splash screen visible
  lcdNeedsFullRefresh = true;

  LOG_INFO("Setup termine. GPIO bouton=%d", PIN_BUTTON);
}


// ═══════════════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════════════

void loop() {
  esp_task_wdt_reset();
  ArduinoOTA.handle();
  mqttclient.loop();

  // ── Bouton (à chaque cycle, pas toutes les 10s !) ──
  handleButton();

  // ── Reconnexion MQTT si nécessaire ──
  if (WiFi.isConnected() && !mqttclient.connected()) {
    tryMqttConnect();
  }

  unsigned long now = millis();

  // ── Phase 1 : lancer conversion température (non-bloquant) ──
  if (firstRun || (now - sensorTimer >= SENSOR_INTERVAL)) {
    sensorTimer = now;
    firstRun    = false;
    sensors.requestTemperatures();
    tempRequestTimer = now;
    tempPending      = true;
  }

  // ── Phase 2 : lire capteurs + publier (~800ms après la demande) ──
  if (tempPending && (now - tempRequestTimer >= 800)) {
    tempPending = false;
    readSensorsAndPublish();
  }

  // ── Affichage pH en temps réel pendant calibration (toutes les 2s) ──
  if (calState == CAL_ACTIVE && (now - calDisplayTimer >= 2000)) {
    calDisplayTimer = now;
    phVoltage = analogRead(PH_PIN) / ESPADC * ESPVOLTAGE;
    phValue   = ph.readPH(phVoltage, temperature);
    char line[17];
    snprintf(line, sizeof(line), "pH: %5.2f       ", phValue);
    lcd.setCursor(0, 1);
    lcd.print(line);
  }
}


// ═══════════════════════════════════════════════════════════════════════════
//  BOUTON UNIQUE — Machine à états
// ═══════════════════════════════════════════════════════════════════════════
//
//  État IDLE :
//    - Appui long 3s  → ENTERPH → passe en CAL_ACTIVE
//    - Appui court    → ignoré (protection)
//
//  État CAL_ACTIVE :
//    - Appui court    → CALPH  → sauvegarde point de calibration
//    - Appui long 3s  → EXITPH → quitte + sauvegarde EEPROM
//    - Timeout 60s    → EXITPH → auto-exit
//
//  Feedback LCD pendant l'appui long : barre de progression "......"

void handleButton() {
  bool btnNow = (digitalRead(PIN_BUTTON) == HIGH);  // pull-down → HIGH = pressé
  unsigned long now = millis();

  // ── Front montant : début d'appui ──
  if (btnNow && !btnPrev) {
    btnPressStart       = now;
    longPressFired      = false;
    lastDotsDisplayed   = -1;
    LOG_DEBUG("Bouton: appui detecte");
  }

  // ── Maintien : barre de progression + détection long press ──
  if (btnNow && !longPressFired) {
    unsigned long held = now - btnPressStart;

    // Afficher la progression après le debounce
    if (held >= DEBOUNCE_MS) {
      int8_t dots = (int8_t)min((unsigned long)6, held / 500UL);

      if (dots != lastDotsDisplayed) {
        lastDotsDisplayed = dots;

        // Construire la ligne de progression (toujours 16 chars)
        char line[17];
        char progress[7] = "      ";
        for (int i = 0; i < dots; i++) progress[i] = '.';

        if (calState == CAL_IDLE) {
          snprintf(line, sizeof(line), "Calibrer? %-6s", progress);
        } else {
          snprintf(line, sizeof(line), "Quitter?  %-6s", progress);
        }
        lcd.setCursor(0, 0);
        lcd.print(line);
      }
    }

    // ── Déclenchement long press à 3s ──
    if (held >= LONGPRESS_MS) {
      longPressFired = true;

      if (calState == CAL_IDLE) {
        // → Entrer en mode calibration
        char cmd[] = "ENTERPH";
        ph.calibration(phVoltage, temperature, cmd);
        calState         = CAL_ACTIVE;
        calActivityTimer = now;
        calPointSaved    = false;
        lcdNeedsFullRefresh = true;

        lcd.clear();
        delay(5);
        //              0123456789012345
        lcd.setCursor(0, 0);
        lcd.print(">> MODE CAL <<  ");
        lcd.setCursor(0, 1);
        lcd.print("Court=save      ");
        LOG_INFO("CAL: mode calibration actif");

      } else {
        // → Quitter le mode calibration + sauvegarder
        char cmd[] = "EXITPH";
        ph.calibration(phVoltage, temperature, cmd);
        calState = CAL_IDLE;
        lcdNeedsFullRefresh = true;

        lcd.clear();
        delay(5);
        lcd.setCursor(0, 0);
        lcd.print(calPointSaved ? "Cal sauvee!     " : "Cal annulee     ");
        lcd.setCursor(0, 1);
        lcd.print("Retour normal...");
        LOG_INFO("CAL: quitte (%s)", calPointSaved ? "sauvegarde EEPROM" : "annulee");
      }
    }
  }

  // ── Front descendant : fin d'appui ──
  if (!btnNow && btnPrev) {
    unsigned long held = now - btnPressStart;

    if (!longPressFired && held >= DEBOUNCE_MS) {
      if (calState == CAL_ACTIVE) {
        // → Appui court en mode CAL = sauvegarder un point
        char cmd[] = "CALPH";
        ph.calibration(phVoltage, temperature, cmd);
        calActivityTimer = now;  // reset du timeout
        calPointSaved    = true;

        lcd.setCursor(0, 0);
        lcd.print("Point sauve!    ");
        lcd.setCursor(0, 1);
        lcd.print("Long=quitter    ");
        LOG_INFO("CAL: point sauvegarde (pH=%.2f)", phValue);
      } else {
        // Appui court en IDLE → ignoré volontairement
        LOG_DEBUG("Bouton: appui court en IDLE, ignore");
      }
    }

    // Restaurer l'affichage normal si on a relâché sans long press
    if (!longPressFired && calState == CAL_IDLE) {
      lcdNeedsFullRefresh = true;
    }
    lastDotsDisplayed = -1;
  }

  // ── Auto-timeout 60s en mode calibration ──
  if (calState == CAL_ACTIVE && (now - calActivityTimer >= CAL_TIMEOUT_MS)) {
    char cmd[] = "EXITPH";
    ph.calibration(phVoltage, temperature, cmd);
    calState = CAL_IDLE;
    lcdNeedsFullRefresh = true;

    lcd.setCursor(0, 0);
    lcd.print(calPointSaved ? "Cal: timeout!   " : "Cal: annulee!   ");
    lcd.setCursor(0, 1);
    lcd.print(calPointSaved ? "Sauvegarde auto " : "Rien sauvegarde ");
    LOG_WARN("CAL: timeout 60s, auto-exit + sauvegarde");
  }

  btnPrev = btnNow;
}


// ═══════════════════════════════════════════════════════════════════════════
//  LECTURE CAPTEURS + PUBLICATION MQTT
// ═══════════════════════════════════════════════════════════════════════════

void readSensorsAndPublish() {
  // ── Lecture PZEM (peut renvoyer NaN si non alimenté) ──
  float voltage   = pzem.voltage();
  float current   = pzem.current();
  float power     = pzem.power();
  float energy    = pzem.energy();
  float frequency = pzem.frequency();
  float pf        = pzem.pf();

  // ── Lecture température + pH ──
  temperature = readTemperature();
  phVoltage   = analogRead(PH_PIN) / ESPADC * ESPVOLTAGE;
  phValue     = ph.readPH(phVoltage, temperature);

  LOG_DEBUG("Temp=%.1f pH=%.2f phV=%.0f", temperature, phValue, phVoltage);

  // ── Mise à jour LCD — UNIQUEMENT en mode IDLE ──
  // En mode CAL, c'est handleButton() qui gère l'écran exclusivement
  if (calState == CAL_IDLE) {
    updateLCD();
  }

  // ── Publication MQTT ──
  if (!WiFi.isConnected() || !mqttclient.connected()) {
    LOG_DEBUG("Publish skip: WiFi=%d MQTT=%d", WiFi.isConnected(), mqttclient.connected());
    return;
  }

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
  LOG_INFO("MQTT: %s", jsonBuffer);
  mqttclient.publish(MQTT_TOPIC_DATA, jsonBuffer, true);
}


// ═══════════════════════════════════════════════════════════════════════════
//  AFFICHAGE LCD
// ═══════════════════════════════════════════════════════════════════════════
//
//  Mode IDLE — affichage normal :
//    Ligne 0 : "CRAZYPOOL   MQTT"  (ou WIFI / NWIF / MERR)
//    Ligne 1 : "25°C      PH:7.2"
//
//  Mode CAL — géré exclusivement par handleButton()
//    Ligne 0 : ">> MODE CAL <<  " / "Point sauve!" / barre progression
//    Ligne 1 : "Court=save" / "Long=quitter" / pH en temps réel

void updateLCD() {
  // Rafraîchissement complet si demandé (changement d'état, boot, etc.)
  if (lcdNeedsFullRefresh) {
    lcdNeedsFullRefresh = false;
    lcd.clear();
    delay(5);
  }

  // ── Ligne 0 : nom + statut connexion ──
  char line0[17];
  const char* status;
  if (!WiFi.isConnected()) {
    status = "NWIF";
  } else if (!mqttclient.connected()) {
    status = "MERR";
  } else {
    status = "MQTT";
  }
  snprintf(line0, sizeof(line0), "CRAZYPOOL   %4s", status);
  lcd.setCursor(0, 0);
  lcd.print(line0);

  // ── Ligne 1 : température + pH ──
  // Format : "25°C      PH:7.2"
  char line1[17];
  char tempStr[5];
  dtostrf(temperature, 2, 0, tempStr);  // ex: "25"

  char phStr[5];
  dtostrf(phValue, 3, 1, phStr);        // ex: "7.2"

  snprintf(line1, sizeof(line1), "%s%cC      PH:%s", tempStr, (char)223, phStr);
  lcd.setCursor(0, 1);
  lcd.print(line1);
}


// ═══════════════════════════════════════════════════════════════════════════
//  TEMPÉRATURE
// ═══════════════════════════════════════════════════════════════════════════

float readTemperature() {
  float t = sensors.getTempCByIndex(0);
  // Protection contre les lectures invalides (-127 = capteur absent)
  if (t == DEVICE_DISCONNECTED_C || t < -10 || t > 60) {
    LOG_WARN("Temp: lecture invalide (%.1f), garde precedente (%.1f)", t, temperature);
    return temperature;  // garder la dernière valeur connue
  }
  return t;
}


// ═══════════════════════════════════════════════════════════════════════════
//  MQTT
// ═══════════════════════════════════════════════════════════════════════════

void tryMqttConnect() {
  if (millis() - mqttRetryTimer < MQTT_RETRY_INTERVAL) return;
  mqttRetryTimer = millis();

  // ID unique basé sur la MAC — évite les conflits multi-ESP32
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "CrazyPool-%08x", (uint32_t)ESP.getEfuseMac());
  LOG_INFO("MQTT: connexion id=%s", clientId);

  // LWT : le broker publie "offline" si l'ESP32 disparaît
  if (mqttclient.connect(clientId, MQTT_USERNAME, MQTT_KEY,
                         MQTT_TOPIC_STATUS, 1, true, "offline")) {
    LOG_INFO("MQTT: connecte !");
    mqttclient.publish(MQTT_TOPIC_STATUS, "online", true);
    lcdNeedsFullRefresh = true;  // mettre à jour le statut sur le LCD
  } else {
    LOG_ERROR("MQTT: echec code=%d, retry dans %lus", mqttclient.state(), MQTT_RETRY_INTERVAL / 1000);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  LOG_INFO("MQTT RX: topic=%s len=%u", topic, length);
  // Traitement des commandes entrantes à implémenter ici
}


// ═══════════════════════════════════════════════════════════════════════════
//  ÉVÉNEMENTS WIFI
// ═══════════════════════════════════════════════════════════════════════════

void onWifiConnected(arduino_event_id_t event, arduino_event_info_t info) {
  LOG_INFO("WiFi: associe au point d'acces");
}

void onWifiGotIP(arduino_event_id_t event, arduino_event_info_t info) {
  LOG_INFO("WiFi: IP = %s", WiFi.localIP().toString().c_str());
  mqttRetryTimer = 0;  // tenter MQTT immédiatement
  lcdNeedsFullRefresh = true;
}

void onWifiDisconnected(arduino_event_id_t event, arduino_event_info_t info) {
  LOG_WARN("WiFi: deconnecte (raison=%d)", info.wifi_sta_disconnected.reason);
  lcdNeedsFullRefresh = true;
  // WiFi.setAutoReconnect(true) gère la reconnexion — rien à faire ici
}
