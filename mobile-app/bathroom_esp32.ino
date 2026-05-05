#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include "DHT.h"

const char* ssid = "Ahmed";
const char* wifi_password = "11223344";

const char* mqtt_server = "e32ad166a4814580bea1d67904922787.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "bathroom_user";
const char* mqtt_password = "Ahmed199";

#define PIR_PIN 4
#define RCWL_PIN 5
#define DHTPIN 6
#define LIGHT_RELAY 7
#define FAN_RELAY 8
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

WiFiClientSecure espClient;
PubSubClient client(espClient);

const unsigned long LIGHT_TIMEOUT = 180000;
const unsigned long MAX_LIGHT_ON_TIME = 900000;

const unsigned long RADAR_WINDOW = 20000;
const int RADAR_PULSE_THRESHOLD = 5;

const unsigned long CONFIRM_WINDOW = 5000;

const float HUM_ON = 80.0;
const float HUM_OFF = 70.0;

// STATE VARIABLES
unsigned long lastMotionTime = 0;
unsigned long radarWindowStart = 0;
unsigned long lightOnStartTime = 0;
unsigned long pirConfirmStart = 0;
unsigned long humidityBelowStartTime = 0;

int radarPulseCount = 0;

bool pirPendingConfirm = false;
bool lightState = false;
bool fanState = false;
bool autoMode = true;

float currentHumidity = 0;
float currentTemperature = 0;

String occupancyState = "VACANT";

// ---------------- LIGHT CONTROL ----------------

void turnLightOn() {
  digitalWrite(LIGHT_RELAY, LOW);
  lightState = true;
  lightOnStartTime = millis();
  publishStates();
}

void turnLightOff() {
  digitalWrite(LIGHT_RELAY, HIGH);
  lightState = false;
  occupancyState = "VACANT";
  publishStates();
}

// ---------------- FAN CONTROL ----------------

void turnFanOn() {
  digitalWrite(FAN_RELAY, LOW);
  fanState = true;
  humidityBelowStartTime = 0;
  publishStates();
}

void turnFanOff() {
  digitalWrite(FAN_RELAY, HIGH);
  fanState = false;
  humidityBelowStartTime = 0;
  publishStates();
}

// ---------------- MQTT ----------------

void publishStates() {
  client.publish("bathroom/light/status", lightState ? "ON" : "OFF", true);
  client.publish("bathroom/fan/status", fanState ? "ON" : "OFF", true);
  client.publish("bathroom/mode/status", autoMode ? "AUTO" : "MANUAL", true);
}

// ---------------- SENSOR FILTERS ----------------

bool radarRead() {
  int count = 0;
  for(int i=0;i<5;i++){
    if(digitalRead(RCWL_PIN)) count++;
    delay(3);
  }
  return count >= 3;
}

bool pirRead() {
  int count = 0;
  for(int i=0;i<7;i++){
    if(digitalRead(PIR_PIN)) count++;
    delay(5);
  }
  return count >= 6;
}

// ---------------- MQTT CALLBACK ----------------

void callback(char* topic, byte* payload, unsigned int length) {

  String message;
  for(int i=0;i<length;i++) message += (char)payload[i];

  if(String(topic)=="bathroom/light/set"){
    autoMode=false;
    if(message=="ON") turnLightOn();
    if(message=="OFF") turnLightOff();
  }

  if(String(topic)=="bathroom/fan/set"){
    autoMode=false;
    if(message=="ON") turnFanOn();
    if(message=="OFF") turnFanOff();
  }

  if(String(topic)=="bathroom/mode/set"){
    if(message=="AUTO") autoMode=true;
    publishStates();
  }
}

// ---------------- WIFI ----------------

void setup_wifi(){
  WiFi.begin(ssid,wifi_password);
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
  }
}

// ---------------- MQTT ----------------

void reconnect(){
  while(!client.connected()){
    if(client.connect("ESP32Bathroom",mqtt_user,mqtt_password)){
      client.subscribe("bathroom/light/set");
      client.subscribe("bathroom/fan/set");
      client.subscribe("bathroom/mode/set");
      publishStates();
    } else {
      delay(2000);
    }
  }
}

// ---------------- SETUP ----------------

void setup(){
  Serial.begin(115200);

  pinMode(PIR_PIN,INPUT_PULLDOWN);
  pinMode(RCWL_PIN,INPUT);
  pinMode(LIGHT_RELAY,OUTPUT);
  pinMode(FAN_RELAY,OUTPUT);

  digitalWrite(LIGHT_RELAY,HIGH);
  digitalWrite(FAN_RELAY,HIGH);

  dht.begin();
  setup_wifi();

  espClient.setInsecure();
  client.setServer(mqtt_server,mqtt_port);
  client.setCallback(callback);

  ArduinoOTA.setPassword("bathroomOTA");
  ArduinoOTA.begin();
}

// ---------------- LOOP ----------------

void loop(){

  ArduinoOTA.handle();

  if(WiFi.status()!=WL_CONNECTED) setup_wifi();
  if(!client.connected()) reconnect();
  client.loop();

  bool pir = pirRead();
  bool radar = radarRead();

  // ENTRY DETECTION

  if(autoMode && pir){
    if(!pirPendingConfirm){
      pirPendingConfirm=true;
      pirConfirmStart=millis();
    }
  }

  if(pirPendingConfirm && radar){
    pirPendingConfirm=false;
    if(!lightState){
      turnLightOn();
      occupancyState="ENTERED";
    }
    lastMotionTime=millis();
  }

  if(pirPendingConfirm && millis()-pirConfirmStart>CONFIRM_WINDOW){
    pirPendingConfirm=false;
  }

  // RADAR EXTENSION

  if(radar && lightState){
    radarPulseCount++;
    if(radarWindowStart==0)
      radarWindowStart=millis();
  }

  if(millis()-radarWindowStart>RADAR_WINDOW){
    if(lightState &&
       radarPulseCount>=RADAR_PULSE_THRESHOLD &&
       millis()-lastMotionTime<60000){

      lastMotionTime=millis();
      occupancyState="PRESENT";
    }
    radarPulseCount=0;
    radarWindowStart=millis();
  }

  // LIGHT SAFETY

  if(lightState && millis()-lightOnStartTime>MAX_LIGHT_ON_TIME){
    turnLightOff();
  }

  if(autoMode && lightState && millis()-lastMotionTime>LIGHT_TIMEOUT){
    turnLightOff();
  }

  // SENSOR UPDATE

  static unsigned long lastRead=0;

  if(millis()-lastRead>2000){

    lastRead=millis();

    currentHumidity=dht.readHumidity();
    currentTemperature=dht.readTemperature();

    if(!isnan(currentHumidity)&&!isnan(currentTemperature)){

      char humBuffer[8];
      dtostrf(currentHumidity,1,2,humBuffer);
      client.publish("bathroom/humidity/status",humBuffer,true);

      char tempBuffer[8];
      dtostrf(currentTemperature,1,2,tempBuffer);
      client.publish("bathroom/temperature/status",tempBuffer,true);

      client.publish("bathroom/motion/status",(pir||radar)?"1":"0",true);
      client.publish("bathroom/pir/status",pir?"1":"0",true);

      char radarBuf[6];
      sprintf(radarBuf,"%d",radarPulseCount);
      client.publish("bathroom/radar/pulses",radarBuf,true);

      client.publish("bathroom/state/status",occupancyState.c_str(),true);

      // -------- FAN LOGIC (SMART DELAY) --------

      if(currentHumidity > HUM_ON && !fanState){
        turnFanOn();
        humidityBelowStartTime = 0;
      }

      if(currentHumidity < HUM_OFF && fanState){

        if(humidityBelowStartTime == 0){
          humidityBelowStartTime = millis();
        }

        if(millis() - humidityBelowStartTime >= 300000){
          turnFanOff();
        }

      } else {
        humidityBelowStartTime = 0;
      }
    }
  }
}