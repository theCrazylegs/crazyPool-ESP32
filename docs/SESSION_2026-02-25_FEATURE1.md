# Session CrazyPool — 25 Février 2026 (suite)
## Feature : wifi-mqtt-nonblocking

> Suite directe de la session du matin.
> Objectif : implémenter l'Étape 1 du plan de refactorisation (stabilité WiFi/MQTT).

---

## Ce qu'on a fait

### Création de la branche

```
git checkout -b feature/wifi-mqtt-nonblocking
```

Depuis `develop`.

---

### Réécriture du .ino — changements principaux

#### Bug #1 supprimé — WiFi bloquant

**Avant :**
```cpp
// Dans loop() — bloquait tout à l'infini
if (WiFi.status() != WL_CONNECTED) {
  while ( WiFi.status() != WL_CONNECTED ) {
    delay ( 500 );
  }
  // ...code mort...
}
```

**Après :**
```cpp
// Dans loop() — ne bloque jamais
if (WiFi.isConnected() && !mqttclient.connected()) {
  tryMqttConnect();  // non-bloquant
}
```

Avec en setup() :
```cpp
WiFi.setAutoReconnect(true);  // le stack WiFi gère tout seul
```

Et dans `onWifiDisconnected` : plus d'appel à `WiFi.begin()` (setAutoReconnect le fait).

---

#### Bug #2 supprimé — MQTT bloquant

**Avant :**
```cpp
void reconnect(){
  while (!mqttclient.connected()) {
    // ...
    delay(2000);  // bloque 2s par tentative
  }
}
```

**Après :**
```cpp
void tryMqttConnect() {
  if (millis() - mqttRetryTimer < MQTT_RETRY_INTERVAL) return;  // gate 5s
  mqttRetryTimer = millis();

  String clientId = "CrazyPool-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  if (mqttclient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_KEY)) {
    // Connecté !
  } else {
    // Echec — on retourne IMMÉDIATEMENT, sans delay()
  }
}
```

Une seule tentative par appel, retour immédiat. La gate `millis()` espace les tentatives à 5 secondes.

---

#### Bug #3 supprimé — Démarrage impossible sans PC

**Avant :**
```cpp
Serial.begin(115200);
while(!Serial);  // attendait indéfiniment sans moniteur série
```

**Après :**
```cpp
Serial.begin(115200);
// PAS de while(!Serial)
```

---

#### Bug #4 supprimé — Boutons bloquants

**Avant :**
```cpp
if (digitalRead(pinBtEnter)){
  ph.calibration(phVoltage,temperature,"ENTERPH");
}
while (digitalRead(pinBtEnter));  // bloquait jusqu'au relâchement
```

**Après :**
```cpp
// handleButtons() — appelée à chaque itération de loop()
bool curEnter = digitalRead(pinBtEnter);
if (curEnter && !lastBtEnter) {  // front montant uniquement
  ph.calibration(phVoltage, temperature, "ENTERPH");
}
lastBtEnter = curEnter;
```

---

### Améliorations bonus

| Amélioration | Détail |
|---|---|
| `mqttclient.loop()` | Absent du code original ! Ajouté en tête de loop() pour le keepalive MQTT |
| ID MQTT unique | `"CrazyPool-" + MAC` — évite les conflits si 2 ESP32 sur le même broker |
| Watchdog timer | `esp_task_wdt_init(30, true)` — reboot auto si loop() bloquée > 30s |
| snprintf + char buffer | Remplace la concaténation String → plus de fragmentation mémoire |
| NaN PZEM géré | `isnan(voltage) ? 0.0f : voltage` — plus de `"nan"` dans le JSON MQTT |
| Code mort supprimé | `mqtt_publish()`, `print_wakeup_reason()`, DeepSleep commenté |

---

### Architecture de loop() après refactorisation

```
loop()
  │
  ├── esp_task_wdt_reset()          ← nourrit le watchdog (30s)
  │
  ├── mqttclient.loop()             ← keepalive MQTT, messages entrants
  │
  ├── handleButtons()               ← edge detection, jamais bloquant
  │
  ├── if (WiFi && !MQTT)            ← tentative MQTT si besoin
  │     └── tryMqttConnect()        ← 1 essai, retour immédiat, gate 5s
  │
  └── if (millis() - timer >= 10s)  ← lecture + publication
        └── readSensorsAndPublish()
              ├── PZEM (NaN géré)
              ├── température
              ├── pH
              ├── LCD update
              └── snprintf JSON → mqttclient.publish()
```

---

### Commit

```
a169050 feat: réécriture WiFi/MQTT non-bloquants, edge detection boutons, watchdog
```

Branche pushée : `feature/wifi-mqtt-nonblocking` sur GitHub.

---

## Prochaine étape

Quand tu auras testé physiquement (ou en simulation) et validé que ça compile et que le comportement est correct :

1. **PR `feature/wifi-mqtt-nonblocking` → `develop`** (via GitHub ou en ligne de commande)
2. Commencer **Étape 2 — `feature/robustness`** :
   - `ArduinoJson` à la place de `snprintf` (déjà partiel)
   - MQTT LWT (Last Will and Testament)
   - Vérification `isnan()` complète (déjà fait ici pour le JSON, à compléter si besoin)

---

## Points à vérifier à la compilation

- `esp_task_wdt.h` : disponible dans Arduino ESP32 board package (ESP-IDF 4.x et 5.x)
  - Si erreur de compilation, commenter les 3 lignes watchdog (`esp_task_wdt_init`, `esp_task_wdt_add`, `esp_task_wdt_reset`)
- `WiFi.isConnected()` : équivalent à `WiFi.status() == WL_CONNECTED`
- `mqttclient.setBufferSize(512)` : nécessite PubSubClient >= 2.7
