# Mes erreurs expliquées — CrazyPool ESP32
## Guide pédagogique pour comprendre ce qui n'allait pas

> Ce document t'explique clairement, sans jargon inutile, pourquoi ton code avait des bugs.
> Tu étais novice — ces erreurs sont extrêmement courantes, même chez des développeurs expérimentés
> qui débutent sur microcontrôleurs. L'important c'est de comprendre POURQUOI.

---

## La grande erreur conceptuelle : penser "PC" sur un "microcontrôleur"

Quand on programme sur PC, si quelque chose bloque pendant 2 secondes, ce n'est pas grave : le PC a plusieurs cœurs, plusieurs processus, le système d'exploitation gère tout en parallèle.

**Sur un ESP32 (Arduino), c'est différent :**

```
Il n'y a qu'une seule chose qui tourne à la fois dans ton programme.
La fonction loop() tourne en boucle, et c'est tout.
Si tu bloques à l'intérieur, TOUT est bloqué.
```

C'est le cœur de TOUS tes bugs. Voyons-les un par un.

---

## Bug #1 — La boucle WiFi infinie
### "Mon ESP32 se bloque et ne se reconnecte jamais au WiFi"

**Ton code :**
```cpp
if (WiFi.status() != WL_CONNECTED) {
  while ( WiFi.status() != WL_CONNECTED ) {
    delay ( 500 );
    Serial.print ( "." );
  }
  // ...
}
```

**Ce que tu pensais faire :**
> "Si le WiFi est coupé, j'attends qu'il revienne en bouclant."

**Ce qui se passe en réalité :**

Imagine que tu es debout devant ta porte d'entrée, et que tu attends que quelqu'un sonne. Tu restes planté là. Tu ne peux rien faire d'autre — pas manger, pas regarder la TV, pas aller aux toilettes. Tu attends.

C'est exactement ce que fait ton ESP32 dans ce `while`. Il est planté là. Il ne peut pas :
- Lire les capteurs
- Mettre à jour l'écran LCD
- Même essayer activement de se reconnecter (c'est l'event handler qui doit le faire)

**Le pire :** Le code après le `while` (le `Serial.println("Connection failed.")` et le `return`) ne sera **jamais exécuté**. Parce que la condition du `while` dit "sort de la boucle quand le WiFi est connecté" — mais si on sort, le WiFi EST connecté, donc on va dans le `else`, pas dans le code "Connection failed". C'est du **code mort** — il ne servira jamais à rien.

**La solution correcte :** Utiliser un *timer* — "si 10 secondes se sont passées et que le WiFi est toujours coupé, je relance une tentative" — sans jamais bloquer.

---

## Bug #2 — La reconnexion MQTT bloquante
### "Mon ESP32 se fige quand le serveur MQTT est injoignable"

**Ton code :**
```cpp
void reconnect(){
  while (!mqttclient.connected()) {
    if (mqttclient.connect("ESPClient", MQTT_USERNAME, MQTT_KEY)) {
      // Connecté !
    } else {
      delay(2000);  // Attend 2 secondes avant de réessayer
    }
  }
}
```

**Ce que tu pensais faire :**
> "Je réessaie toutes les 2 secondes jusqu'à ce que ça marche."

**Ce qui se passe en réalité :**

Même problème. Si ton serveur MQTT (Mosquitto sur ton NAS ou Raspberry Pi) est éteint ou redémarre, cette boucle tourne indéfiniment avec des pauses de 2 secondes. Pendant ce temps, rien d'autre ne fonctionne.

Et cette fonction est appelée depuis la boucle principale toutes les 10 secondes ! Donc si MQTT est mort, ton ESP32 passe son temps dans cette boucle.

**Ce que ça donnait en pratique :**
1. Tu redémarres ton serveur MQTT
2. L'ESP32 entre dans `reconnect()`
3. Ça ne répond pas pendant 2s, 4s, 6s...
4. LCD figé, capteurs non lus, plus de mises à jour
5. Tu vois le WiFi "casser" alors que c'est MQTT qui bloque

---

## Bug #3 — L'oubli du déploiement standalone
### "Mon ESP32 ne démarre pas quand il est branché sur un chargeur"

**Ton code :**
```cpp
Serial.begin(115200);
while(!Serial);  // ← LE COUPABLE
```

**Ce que tu pensais faire :**
> C'est une ligne que tu as copiée d'un tutoriel pour "attendre que le moniteur série soit prêt".

**Ce qui se passe en réalité :**

Sur Arduino UNO, `while(!Serial)` attend que le port USB série soit ouvert par un programme (comme le Moniteur Série de l'IDE Arduino). Ça a du sens pendant le développement.

Sur ESP32, en standalone (branché sur un chargeur USB sans PC), `Serial` n'est jamais "prêt" au sens de ce test. L'ESP32 attend indéfiniment. Il ne démarre jamais.

**Comment tu l'as découvert (probablement) :**
> "Ça marche quand j'ai le câble USB branché au PC, mais pas quand je le branche sur un chargeur..."

C'est ça. La solution : simplement supprimer ce `while(!Serial)`. Il n'a aucune utilité sur ESP32 en prod.

---

## Bug #4 — Les boutons qui "gèlent" tout
### "Quand j'appuie sur un bouton, l'écran se fige"

**Ton code :**
```cpp
if (digitalRead(pinBtEnter)){
  ph.calibration(phVoltage,temperature,"ENTERPH");
}
while (digitalRead(pinBtEnter));  // ← Attend que tu lâches
```

**Ce que tu pensais faire :**
> "J'attends que le bouton soit relâché pour éviter les rebonds (répétition de l'action)."

**Ce qui se passe en réalité :**

L'idée de base est bonne (éviter qu'une pression courte déclenche 100 fois l'action). Mais l'implémentation bloque tout pendant que tu maintiens le bouton appuyé.

Il existe une meilleure technique : la **détection du front montant** (edge detection). On détecte uniquement le moment où le bouton PASSE de "non appuyé" à "appuyé", pas toute la durée de la pression.

---

## Bug #5 — Les valeurs "NaN" du compteur PZEM
### "Home Assistant affiche 'nan' pour la consommation électrique"

**Ton code :**
```cpp
float voltage = pzem.voltage();   // Peut renvoyer NaN
float current = pzem.current();   // Peut renvoyer NaN
// ... direct en MQTT sans vérification
```

**Ce que tu pensais faire :**
> "Je lis le compteur et j'envoie les données."

**Ce qui se passe en réalité :**

La librairie PZEM renvoie `NaN` (Not a Number — une valeur spéciale qui signifie "pas de données valides") quand :
- Le compteur n'est pas connecté à du courant alternatif
- La communication série est perturbée
- Le compteur est en train de démarrer

`NaN` converti en texte donne la chaîne `"nan"`. Ton JSON MQTT ressemble alors à :
```json
{"temperature": "24.5", "Volt": "nan", "Ampere": "nan", ...}
```

Home Assistant ne sait pas quoi faire avec `"nan"` et affiche une erreur ou une valeur invalide.

**La solution :** Vérifier avec `isnan()` avant d'utiliser la valeur :
```cpp
if (!isnan(voltage)) {
  // Utiliser voltage
} else {
  // Valeur par défaut ou ne pas envoyer
}
```

---

## Bug #6 — L'ID MQTT qui déconnecte les appareils
### "Mon ESP32 se déconnecte tout seul du MQTT"

**Ton code :**
```cpp
mqttclient.connect("ESPClient", MQTT_USERNAME, MQTT_KEY)
//                  ^^^^^^^^^^ Toujours le même ID
```

**Ce que tu pensais faire :**
> "Je donne un nom à mon client MQTT."

**Ce qui se passe en réalité :**

Le protocole MQTT dit : **deux clients ne peuvent pas avoir le même identifiant sur le même broker**. Si un 2ème client se connecte avec le même ID, le broker **déconnecte le 1er**.

Dans la pratique, si ton ESP32 plante et redémarre alors que l'ancienne connexion est encore "vivante" côté broker (pendant quelques secondes), il peut se déconnecter lui-même en essayant de se reconnecter. C'est une source de boucle de déconnexions.

**La solution :** Utiliser l'adresse MAC de l'ESP32 comme partie de l'ID, garantissant l'unicité :
```cpp
String clientId = "CrazyPool-" + String((uint32_t)ESP.getEfuseMac(), HEX);
mqttclient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_KEY);
```

---

## Bug #7 — La fragmentation mémoire (le bug invisible)
### "Mon ESP32 plante aléatoirement après quelques heures"

**Ton code :**
```cpp
jsontomqtt = "{\"temperature\": \"" + String(temperature) +
             "\", \"ph\": \"" + String(phValue) +
             "\", \"Volt\": \"" + String(voltage) + ...
```

**Ce que tu pensais faire :**
> "Je construis mon JSON en assemblant des morceaux de texte."

**Ce qui se passe en réalité :**

Sur un PC, cette opération est triviale. Sur un ESP32 avec 320 Ko de RAM (et souvent moins de 200 Ko disponibles pour ton programme), chaque `+` entre chaînes :
1. Alloue une nouvelle zone mémoire pour le résultat
2. Copie les deux parties dedans
3. Libère l'ancienne zone

Répété des dizaines de fois par opération, des centaines de fois par heure, la mémoire se fragmente. Imagine un puzzle : même si tu as assez de pièces totales, si elles ne sont pas contiguës, tu ne peux pas placer une grande pièce.

Résultat : après plusieurs heures, l'allocation échoue silencieusement et l'ESP32 peut rebooter.

**La solution :** Utiliser la librairie `ArduinoJson` qui gère tout ça efficacement :
```cpp
StaticJsonDocument<256> doc;
doc["temperature"] = temperature;
doc["ph"] = phValue;
doc["Volt"] = voltage;
// ... serializeJson(doc, buffer);
```

---

## Bug #8 — Les credentials dans le code source
### "Mes mots de passe WiFi étaient sur GitHub !"

**Ton code original :**
```cpp
const char* ssid = "MonWifi";       // ← Visible sur GitHub !
const char* password = "MonMotDePasse";  // ← Visible sur GitHub !
```

**Ce que tu pensais faire :**
> "C'est un projet perso, je mets les credentials directement."

**Ce qui se passe en réalité :**

GitHub est public. N'importe qui dans le monde peut lire ton code. Des robots scannent GitHub en permanence à la recherche de credentials (mots de passe, clés API, etc.).

Dans ton cas, les credentials étaient **vides** sur le repo (tu avais au moins pensé à vider les champs avant de pousher), mais c'est une mauvaise pratique qui peut mener à une erreur un jour.

**La solution qu'on a mise en place :**
- `secrets.h` contient les vraies valeurs → dans `.gitignore`, jamais pushé
- `secrets.h.example` contient des valeurs bidon → pushé comme documentation

---

## Résumé visuel de tous les bugs

```
┌─────────────────────────────────────────────────────────────────┐
│                     BOUCLE PRINCIPALE (loop)                    │
│                                                                  │
│  Toutes les 10s :                                               │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 1. Lire PZEM    ← peut renvoyer NaN (bug #5)            │   │
│  │ 2. Bouton ?     ← bloque si appuyé (bug #4)             │   │
│  │ 3. Lire temp    OK                                       │   │
│  │ 4. Lire pH      OK                                       │   │
│  │ 5. WiFi ok ?                                             │   │
│  │    └── NON → ██████ BLOQUE TOUT ██████ (bug #1)         │   │
│  │    └── OUI → MQTT connecté ?                            │   │
│  │              └── NON → ██ BLOQUE TOUT ██ (bug #2)       │   │
│  │              └── OUI → Publier JSON                     │   │
│  │                        └── String + → fragmente (bug #7)│   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                  │
│  Démarrage :                                                     │
│  while(!Serial) ← BLOQUE si pas de PC (bug #3)                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## La leçon principale

> **Sur microcontrôleur, on ne "bloque" JAMAIS. On "programme l'avenir".**

Au lieu de :
```cpp
// MAUVAIS - style "PC"
while(quelqueChoseNEstPasPret) {
  delay(500);
  réessayer();
}
```

On fait :
```cpp
// BON - style "microcontrôleur"
if (millis() - dernierEssai > 500) {
  dernierEssai = millis();
  réessayer();
  // On rend la main immédiatement, on reviendra dans 500ms
}
```

`millis()` renvoie le nombre de millisecondes depuis le démarrage de l'ESP32. C'est l'outil fondamental pour créer des délais **non-bloquants**.

---

## Tu n'étais pas si loin !

Malgré ces bugs, tu avais réussi à :
- Faire fonctionner 3 capteurs différents (pH, température, énergie) ensemble
- Mettre en place la communication MQTT
- Gérer un écran LCD
- Implémenter une calibration pH avec boutons
- Déployer quelque chose qui "marchait à peu près"

C'est déjà beaucoup pour un premier projet IoT ! Les bugs que tu avais sont des classiques que tout le monde fait en passant du développement PC au développement embarqué.

La prochaine étape va consister à réécrire le "squelette" du code (la gestion WiFi/MQTT) avec les bonnes pratiques, sans toucher à la logique des capteurs que tu avais bien faite.
