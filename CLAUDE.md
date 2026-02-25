# CLAUDE.md — CrazyPool ESP32

Ce fichier donne le contexte complet du projet à Claude pour travailler efficacement.

---

## Projet

**CrazyPool** est une piscine connectée DIY basée sur ESP32.
Surveillance en temps réel de la température, du pH et de la consommation électrique.
Les données sont publiées via MQTT et seront intégrées à Home Assistant.

---

## Matériel

| Composant | Modèle | Broche ESP32 |
|---|---|---|
| Sonde pH | DFRobot SEN0161-V2 (3.3V) | GPIO 33 (analogique) |
| Sonde temp | DFRobot DFR0198 / DS18B20 (5V) | GPIO 13 (OneWire) |
| Compteur énergie | PZEM-004T v3.0 | Serial2 (RX2/TX2) |
| Écran | LCD 16x2 (HD44780) | GPIO 23,22,21,19,18,5 |
| Bouton ENTER (pH cal) | Poussoir | GPIO 4 |
| Bouton CAL (pH cal) | Poussoir | GPIO 2 |
| Bouton EXIT (pH cal) | Poussoir | GPIO 15 |

---

## Librairies Arduino requises

```
DFRobot_ESP_PH
EEPROM (built-in ESP32)
OneWire
DallasTemperature
LiquidCrystal
PZEM004Tv30
WiFi (built-in ESP32)
PubSubClient
```

---

## Structure des fichiers

```
crazyPool-ESP32/
├── crazyPool-ESP32.ino   ← Code principal
├── secrets.h             ← Credentials (GITIGNORE - ne pas committer !)
├── secrets.h.example     ← Template vide pour nouveaux devs
├── .gitignore            ← Exclut secrets.h
├── CLAUDE.md             ← Ce fichier
└── README.md             ← Documentation utilisateur
```

---

## Credentials (secrets.h)

Le fichier `secrets.h` est **gitignore** et contient :
- `WIFI_SSID` / `WIFI_PWD` — identifiants WiFi
- `MQTT_BROKER` / `MQTT_BROKER_PORT` / `MQTT_USERNAME` / `MQTT_KEY` — broker MQTT local

Voir `secrets.h.example` pour le template.

---

## Stratégie Git

```
main      ← code stable/production (ce qui tourne sur l'ESP32)
develop   ← branche de travail quotidienne  ← ON EST ICI
feature/  ← branches créées depuis develop pour chaque correctif/feature
```

**Workflow :**
1. Travailler sur `develop` ou créer une branche `feature/nom`
2. Pull Request vers `develop` quand c'est stable
3. Merge `develop` → `main` uniquement quand tout est validé

---

## Bugs connus (état au 25/02/2026)

### CRITIQUE - À corriger en priorité

1. **Boucle WiFi bloquante** (`loop()`) — quand WiFi coupe, `while(WiFi.status() != WL_CONNECTED)` bloque tout à l'infini avec `delay(500)`. Capteurs et LCD s'arrêtent.

2. **reconnect() MQTT bloquant** — `while(!mqttclient.connected())` avec `delay(2000)` bloque tout pendant les tentatives MQTT.

3. **`while(!Serial)` bloque sans PC** — empêche le démarrage standalone.

4. **Boutons bloquants** — `while(digitalRead(pinBt...))` bloque jusqu'au relâchement.

### MINEUR

5. **Valeurs PZEM sans vérif NaN** — si PZEM débranché, `pzem.voltage()` renvoie NaN publié tel quel en MQTT.
6. **ID MQTT hardcodé "ESPClient"** — conflit si 2 appareils sur le même broker.
7. **Fragmentation mémoire** — concaténation `String` dans la boucle principale.
8. **Code mort** — `mqtt_publish()`, `print_wakeup_reason()`, code DeepSleep jamais utilisés.

---

## Plan de refactorisation (Etapes)

### Etape 1 — Stabilité WiFi/MQTT (priorité absolue)
- Remplacer les boucles bloquantes par une **machine à états non-bloquante** avec `millis()`
- `WiFi.setAutoReconnect(true)` + backoff exponentiel
- MQTT non-bloquant avec tentatives espacées
- Supprimer `while(!Serial)`
- Ajouter watchdog timer (WDT)

### Etape 2 — Robustesse
- Vérification `isnan()` sur toutes les valeurs PZEM
- Remplacer `String` par `ArduinoJson` (pas de fragmentation mémoire)
- MQTT Last Will and Testament (LWT) pour détecter offline
- ID MQTT unique basé sur MAC address

### Etape 3 — Sécurité & Maintenance
- MQTT over TLS (port 8883)
- OTA (Over The Air updates)
- Logs structurés (niveau DEBUG/INFO/ERROR)

### Etape 4 — Home Assistant
- Topics MQTT compatibles HA auto-discovery
- Dashboard avec température, pH, consommation électrique
- Alertes (pH hors plage, température anormale)

---

## Contexte Home Assistant

Home Assistant a été désinstallé - à réinstaller ultérieurement.
Le broker MQTT (Mosquitto) était sur `192.168.2.235` (à vérifier).
L'intégration HA se fera via auto-discovery MQTT une fois le code ESP32 stabilisé.

---

## Notes de développement

- L'ESP32 a un ADC non-linéaire sur GPIO 33 (sonde pH) — compensation à prévoir
- La sonde pH est **industrielle** (résistante à la pression) — différente du modèle lab DFRobot
- Le PZEM-004T utilise Serial2 par défaut sur ESP32
- Les boutons poussoirs sont en `INPUT` sans résistance de pull-up/pull-down — à vérifier selon le câblage
