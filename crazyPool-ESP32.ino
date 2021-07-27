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
#include <LiquidCrystal.h>
#include <PZEM004Tv30.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>

//WIFI
const char* ssid = "WifiLoveMachine 2.4"; // Enter your WiFi name
const char* password =  "bontempshome"; // Enter WiFi password

//MQTT
#define MQTT_BROKER       "192.168.2.235"
#define MQTT_BROKER_PORT  1883
#define MQTT_USERNAME     "mqtt"
#define MQTT_KEY          "mqtt" 

long tps=0;
String jsontomqtt;



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
WiFiMulti wifiMulti;
WiFiClient espClient;
PubSubClient client(espClient);


void setup() {
  Serial.begin(115200);
  while(!Serial);
  
  setup_wifi();
  setup_mqtt();


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




  // GET PH
  static unsigned long timepoint = millis();
  if (millis() - timepoint > 1000U) //time interval: 1s
  {
    timepoint = millis();
    
   //   
  //    // ATTENTION A FAIRE lors du calibrage QUID de la temperature des  solution de Ph 4 et 7 qui doivent être à 25°c
      temperature = readTemperature();         // on lit la temperature de l'eau de la piscine

    //voltage = rawPinValue / esp32ADC * esp32Vin
        phVoltage = analogRead(PH_PIN) / ESPADC * ESPVOLTAGE; // read the voltage Arduino ESP32     
 //     phVoltage = analogRead(PH_PIN)/1024.0*5000;  // read the voltage version Arduino UNO
        phValue = ph.readPH(phVoltage,temperature);  // convert voltage to pH with temperature compensation
      
     
    // on met les données dans un JSON
    jsontomqtt = "{\"temperature\": " + String(temperature) +", \"ph\": " + String(phValue) +", \"pressure\": " + String(pressure) +"}";

    // on publie le JSON sur le broket
    client.publish("esp32/crazypool", (char*) jsontomqtt.c_str(), true);


     // Serial.print("temperature:");
      //Serial.print(temperature,1);
     // Serial.print("°C  pH:");
     // Serial.println(phValue,1);
      
      lcd.setCursor(0,1);
      lcd.print(temperature,0);
      lcd.setCursor(13,1);
      lcd.print(phValue,1);
  }
  //ph.calibration(phVoltage, temperature); // calibration process by Serail CMD



  //Serial.println();
  delay(100);
}





float readTemperature(){
  
  //returns the temperature from one DS18S20 in DEG Celsius
/*
  byte data[12];
  byte addr[8];

  if ( !ds.search(addr)) {
      //no more sensors on chain, reset search
      ds.reset_search();
      return -1000;
  }

  if ( OneWire::crc8( addr, 7) != addr[7]) {
      Serial.println("CRC is not valid!");
      return -1000;
  }

  if ( addr[0] != 0x10 && addr[0] != 0x28) {
      Serial.print("Device is not recognized");
      return -1000;
  }

  ds.reset();
  ds.select(addr);
  ds.write(0x44,1); // start conversion, with parasite power on at the end

  byte present = ds.reset();
  ds.select(addr);
  ds.write(0xBE); // Read Scratchpad


  for (int i = 0; i < 9; i++) { // we need 9 bytes
    data[i] = ds.read();
  }

  ds.reset_search();

  byte MSB = data[1];
  byte LSB = data[0];

  float tempRead = ((MSB << 8) | LSB); //using two's compliment
  float TemperatureSum = tempRead / 16;
  return TemperatureSum;
  */

/* avec Dallas*/
  
  sensors.requestTemperatures(); 
  float temperatureC = sensors.getTempCByIndex(0);
  return temperatureC;
  

  

}


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
}
 
void setup_mqtt(){
  client.setServer(MQTT_BROKER, MQTT_BROKER_PORT);
  client.setCallback(callback);//Déclaration de la fonction de souscription
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
  while (!client.connected()) {
    Serial.println("Connection au serveur MQTT ...");
    if (client.connect("ESPClient", MQTT_USERNAME, MQTT_KEY)) {
      Serial.println("MQTT connecté");
    }
    else {
      Serial.print("echec, code erreur= ");
      Serial.println(client.state());
      Serial.println("nouvel essai dans 2s");
    delay(2000);
    }
  }
  client.subscribe("esp/test/led");//souscription au topic led pour commander une led
}
 
//Fonction pour publier un float sur un topic 
void mqtt_publish(String topic, float t){
  char top[topic.length()+1];
  topic.toCharArray(top,topic.length()+1);
  char t_char[50];
  String t_str = String(t);
  t_str.toCharArray(t_char, t_str.length() + 1);
  client.publish(top,t_char);
}


