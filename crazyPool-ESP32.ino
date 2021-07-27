/*
CRAZYPOOL V1
- ESP32 
- Sonde PH DFRobot SKU:SEN0161-V2 en 3v3
  ->  3 Bouton Poussoir pour le Calibrage de la sonde PH
- Sonde Temp DFRobot SKU:DFR0198 (DS18B20) sur ESP32 5v et non 3v3
- Ecran LCD Pour Affichage de la Temperature et du PH
- Energy PZEM-004t
*/

#include <DFRobot_ESP_PH.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal.h> // This library allows an Arduino board to control liquid crystal displays (LCDs) based on the Hitachi HD44780 (or a compatible) chipset
#include <PZEM004Tv30.h>
#include <WiFi.h>
#include <PubSubClient.h>


//DeepSleep
//#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
//#define TIME_TO_SLEEP  60        /* Time ESP32 will go to sleep (in seconds) */
//RTC_DATA_ATTR int bootCount = 0; // compteur de réveil


//WIFI
const char* ssid = ""; // Enter your WiFi name
const char* password =  ""; // Enter WiFi password

//MQTT
#define MQTT_BROKER       "" // Enter your IP MQTT BROKER
#define MQTT_BROKER_PORT  1883
#define MQTT_USERNAME     ""  // Enter your Username MQTT BROKER
#define MQTT_KEY          ""   // Enter your Password MQTT BROKER

long tps=0;
String jsontomqtt;
int boucle1 = 0;



// Init PIN
#define pinBtEnter 4
#define pinBtCal 2
#define pinBtExit 15
#define PH_PIN 33 // fil Bleu
#define DS18B20_Pin 13 //DS18B20 Signal Temperature // fil VERT



// Init Value
float phVoltage,phValue,temperature = 25;
bool bDisplayPh = true;

#define ESPADC 4096.0   //the esp Analog Digital Convertion value
#define ESPVOLTAGE 3300 //the esp voltage supply value


// Init Object
DFRobot_ESP_PH ph;
PZEM004Tv30 pzem(&Serial2);
LiquidCrystal lcd(23,22,21,19,18,5); // quel num broche vont communiquer
OneWire ds(DS18B20_Pin); // sonde Temp
DallasTemperature sensors(&ds); // version ESP32 sonde Temp


// Init Wifi
WiFiClient espClient;
PubSubClient mqttclient(espClient);





void setup() {
  Serial.begin(115200);
  while(!Serial);

  WiFi.disconnect(true);
  delay(1000);

  WiFi.onEvent(Wifi_connected,SYSTEM_EVENT_STA_CONNECTED);
  WiFi.onEvent(Get_IPAddress, SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(Wifi_disconnected, SYSTEM_EVENT_STA_DISCONNECTED); 
  WiFi.begin(ssid, password);
  Serial.println("Waiting for WIFI network...");



  //Increment boot number and print it every reboot
  //++bootCount;
  //Serial.println("Boot number: " + String(bootCount));

  //Print the wakeup reason for ESP32
  //print_wakeup_reason();

  /*
  First we configure the wake up source
  We set our ESP32 to wake up every X seconds
  */
  //esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
 // Serial.println("ESP32 réveillé dans " + String(TIME_TO_SLEEP) + " seconds");

  
  //setup_wifi();
  //setup_mqtt();


  EEPROM.begin(32);//needed to permit storage of calibration value in eeprom for Ph

  
  lcd.begin(16,2); //taille du LCD
  lcd.print("CRAZYPOOL");
  lcd.setCursor(2,1);
  lcd.print((char) 223);
  lcd.setCursor(3,1);
  lcd.print("C");  
  lcd.setCursor(10,1);
  lcd.print("PH:");
  

  ph.begin(); // Sonde Ph
  sensors.begin(); // Sonde Temp 

  pinMode(pinBtEnter, INPUT);
  pinMode(pinBtCal,   INPUT);
  pinMode(pinBtExit,  INPUT);



}

void loop() {
  
  // toutes les secondes
  static unsigned long timepoint = millis();
  if (millis() - timepoint > 10000U || boucle1 == 0) //time interval: 10s // on force un premier passage dès le début
  {
    timepoint = millis();
    
    boucle1 = 1;

    Serial.println(mqttclient.connected());

    //Energy Analyse
    float voltage = pzem.voltage(); //V
    float current = pzem.current(); //A
    float power = pzem.power(); //W
    float energy = pzem.energy(); //kWh
    float frequency = pzem.frequency(); //Hz
    float pf = pzem.pf(); // power factor




    /*
    if( !isnan(voltage) ){
        Serial.print("Voltage: "); Serial.print(voltage); Serial.println("V");
    }

    if( !isnan(power) ){
        Serial.print("Power: "); Serial.print(power); Serial.println("W");
    }
    */



    // CALIBRATION par Bouton pourrsoir
    if (digitalRead(pinBtEnter)){
      ph.calibration(phVoltage,temperature,"ENTERPH"); 
      lcd.setCursor(9,1);
      lcd.print("CAL:");
    }  
    while (digitalRead(pinBtEnter));


    // ON FIXE LA CALIBRATION PH4 / PH7
    if (digitalRead(pinBtCal)){
      ph.calibration(phVoltage,temperature,"CALPH"); 
      lcd.setCursor(12,0);
      lcd.print("save");
    }
    while (digitalRead(pinBtCal));


    // ON SAUVE ET ON SORT DU MODE
    if (digitalRead(pinBtExit)){
      ph.calibration(phVoltage,temperature,"EXITPH"); 
      lcd.setCursor(12,0);
      lcd.print("    ");   
      lcd.setCursor(9,1);
      lcd.print(" PH:");     
    }
    while (digitalRead(pinBtExit));   



    //   
    //    // ATTENTION A FAIRE lors du calibrage QUID de la temperature des  solution de Ph 4 et 7 qui doivent être à 25°c
    temperature = readTemperature();         // on lit la temperature de l'eau de la piscine

    //voltage = rawPinValue / esp32ADC * esp32Vin
    phVoltage = analogRead(PH_PIN) / ESPADC * ESPVOLTAGE; // read the voltage Arduino ESP32     
    //     phVoltage = analogRead(PH_PIN)/1024.0*5000;  // read the voltage version Arduino UNO
    phValue = ph.readPH(phVoltage,temperature);  // convert voltage to pH with temperature compensation



    Serial.print("temperature:");
    Serial.println(temperature,1);

    // Serial.print("°C  pH:");
    // Serial.println(phValue,1);

    lcd.setCursor(0,1);
    lcd.print(temperature,0);
    lcd.setCursor(13,1);
    lcd.print(phValue,1);
    //ph.calibration(phVoltage, temperature); // calibration process by Serail CMD



    if (WiFi.status() != WL_CONNECTED) {
      
      while ( WiFi.status() != WL_CONNECTED ) {
        delay ( 500 );
        Serial.print ( "." );
      }

      Serial.println("Connection failed.");
      Serial.println("Waiting 5 seconds before retrying...");
      delay(5000);
      return;

    }else{

      if (!mqttclient.connected()) {
        reconnect();
      }
     
      // on met les données dans un JSON
      jsontomqtt = "{\"temperature\": \"" + String(temperature) +"\", \"ph\": \"" + String(phValue) +"\", \"Volt\": \"" + String(voltage) +"\", \"Ampere\": \"" + String(current) +"\", \"Watts\": \"" + String(power) +"\", \"Kwh\": \"" + String(energy) +"\", \"Hz\": \"" + String(frequency) +"\", \"Power_factor\": \"" + String(pf) +"\"}";

      Serial.println(jsontomqtt);

      // on publie le JSON sur le broket
      mqttclient.publish("esp32/crazypool", (char*) jsontomqtt.c_str(), true);

      //Rentre en mode Deep Sleep
      //Serial.println("Rentre en mode Light Sleep");
      //Serial.println("----------------------");
      //delay(100);
      //esp_light_sleep_start();
      


    }


    


  }

  //Serial.println();
 // delay(100);
}





float readTemperature(){
  
  /* avec Dallas*/
  
  sensors.requestTemperatures(); 
  float temperatureC = sensors.getTempCByIndex(0);
  return temperatureC;  

}

/*
void setup_wifi(){
  //connexion au wifi
  wifiMulti.addAP(ssid, password);
  while ( wifiMulti.run() != WL_CONNECTED ) {
    delay ( 500 );
    Serial.print ( "." );
  }
  Serial.println("");
  Serial.println("WiFi connecté");
  Serial.print("MAC : ");
  Serial.println(WiFi.macAddress());
  Serial.print("Adresse IP : ");
  Serial.println(WiFi.localIP());
}*/
 
void setup_mqtt(){
  mqttclient.setServer(MQTT_BROKER, MQTT_BROKER_PORT);
  mqttclient.setCallback(callback);//Déclaration de la fonction de souscription
  reconnect();
}

 
//Callback doit être présent pour souscrire a un topic et de prévoir une action 
void callback(char* topic, byte *payload, unsigned int length) {
   Serial.println("-------Nouveau message du broker mqtt-----");
   Serial.print("Canal:");
   Serial.println(topic);
   Serial.print("donnee:");
   Serial.write(payload, length);
   Serial.println();
   if ((char)payload[0] == '1') {
     Serial.println("LED ON");
      digitalWrite(2,HIGH); 
   } else {
     Serial.println("LED OFF");
     digitalWrite(2,LOW); 
   }
 }
 
 
void reconnect(){
  while (!mqttclient.connected()) {
    Serial.println("Connection au serveur MQTT ...");
    if (mqttclient.connect("ESPClient", MQTT_USERNAME, MQTT_KEY)) {
      Serial.println("MQTT connecté");
      lcd.setCursor(12,0);
      lcd.print("MQTT");
    }
    else {
      Serial.print("echec, code erreur= ");
      Serial.println(mqttclient.state());
      Serial.println("nouvel essai dans 2s");
      lcd.setCursor(12,0);
      lcd.print("ERR2");
    delay(2000);
    }
  }
  //mqttclient.subscribe("esp/test/led");//souscription au topic led pour commander une led
}
 
//Fonction pour publier un float sur un topic 
void mqtt_publish(String topic, float t){
  char top[topic.length()+1];
  topic.toCharArray(top,topic.length()+1);
  char t_char[50];
  String t_str = String(t);
  t_str.toCharArray(t_char, t_str.length() + 1);
  mqttclient.publish(top,t_char);
}



//events Wifi
void Wifi_connected(WiFiEvent_t event, WiFiEventInfo_t info){
  Serial.println("Successfully connected to Access Point");
}

void Get_IPAddress(WiFiEvent_t event, WiFiEventInfo_t info){
  Serial.println("WIFI is connected!");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  lcd.setCursor(12,0);
  lcd.print("WIFI");

  setup_mqtt(); // on se connecte au serveur MQTT
}

void Wifi_disconnected(WiFiEvent_t event, WiFiEventInfo_t info){

  lcd.setCursor(12,0);
  lcd.print("ERR1");

  Serial.println("Disconnected from WIFI access point");
  Serial.print("WiFi lost connection. Reason: ");
  Serial.println(info.disconnected.reason);
  Serial.println("Reconnecting...");
  WiFi.begin(ssid, password);
}


void print_wakeup_reason(){
   esp_sleep_wakeup_cause_t source_reveil;

   source_reveil = esp_sleep_get_wakeup_cause();

   switch(source_reveil){
      case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Réveil causé par un signal externe avec RTC_IO"); break;
      case ESP_SLEEP_WAKEUP_EXT1 : Serial.println("Réveil causé par un signal externe avec RTC_CNTL"); break;
      case ESP_SLEEP_WAKEUP_TIMER : Serial.println("Réveil causé par un timer"); break;
      case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Réveil causé par un touchpad"); break;
      default : Serial.printf("Réveil pas causé par le Deep Sleep: %d\n",source_reveil); break;
   }
}
