#include "SparkFunHTU21D.h"
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <SimpleTimer.h>
#include <RCSwitch.h>h
#include "settings.h"
#include <queue>
#include <Bounce2.h>

#define BUTTON_PIN D5 // (=D5)

uint8_t mqttBaseTopicSegmentCount = 0;
uint8_t mqttRetryCounter = 0;

char convertBuffer[10] = {0};

typedef struct {
  char systemCode[6] = {0};
  char unitCode[6] = {0};
  bool on;
} rcJob;

std::queue<rcJob> rcJobQueue;

unsigned long nextJobMillis = 0;

WiFiClient wifiClient;
PubSubClient mqttClient;

SimpleTimer timer;
HTU21D htu21;
RCSwitch rcSwitch = RCSwitch();

Bounce debouncer1 = Bounce();
bool buttonStateLatest1 = false;
uint8_t echoTopic = ECHO_TOPIC;

void mqttConnect() {
  
  while (!mqttClient.connected()) {
    Serial.println("Connecting MQTT");
    if (mqttClient.connect(HOSTNAME, MQTT_TOPIC_STATE, 1, true, "disconnected")) {
      mqttClient.subscribe(MQTT_TOPIC_RCSWITCH);
      mqttClient.subscribe(MQTT_TOPIC_MQTTESP);
      mqttClient.subscribe(MQTT_TOPIC_LED_1);
      mqttClient.subscribe(MQTT_TOPIC_LED_2);
      mqttClient.subscribe(MQTT_TOPIC_ECHO);
      mqttClient.publish(MQTT_TOPIC_STATE, "connected", true);
      mqttRetryCounter = 0;
      
    } else {
      Serial.println("MQTT connect failed!");
      
      if (mqttRetryCounter++ > MQTT_MAX_CONNECT_RETRY) {
        ESP.restart();
      }
      
      delay(2000);
    }
  }
}

bool isCodeValid(char* code) {

  if (strlen(code) != 5) {
    return false;
  }

  for (uint8_t i = 0; i < 5; i++) {
    if (code[i] != '0' && code[i] != '1') {
      return false;
    }
  }

  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  
  uint8_t segment = 0;
  char *token;
  rcJob job;

  //char* origTopic = topic;
  String origTopic = topic;

  // MQTT-EchoTopic-Switch
  if (strncmp(topic, MQTT_TOPIC_ECHO, strlen(MQTT_TOPIC_ECHO)) == 0) {
    if (strncmp((char*) payload, "ON", length) == 0) {
      echoTopic = true;
    } else if (strncmp((char*) payload, "OFF", length) == 0) {
      echoTopic = false;
    }
    return;
  }

  // MQTT-Connection-Indicator-LED
  if (strncmp(topic, MQTT_TOPIC_MQTTESP, strlen(MQTT_TOPIC_MQTTESP)) == 0) {
    if (strncmp((char*) payload, "ON", length) == 0) {
      digitalWrite(BUILTIN_LED, LOW);
    } else if (strncmp((char*) payload, "OFF", length) == 0) {
      digitalWrite(BUILTIN_LED, HIGH);
    }
    return;
  }

  // Builtin LED1 Switch
  if (strncmp(topic, MQTT_TOPIC_LED_1, strlen(MQTT_TOPIC_LED_1)) == 0) {
    if (strncmp((char*) payload, "ON", length) == 0) {
      digitalWrite(BUILTIN_LED, LOW);
    } else if (strncmp((char*) payload, "OFF", length) == 0) {
      digitalWrite(BUILTIN_LED, HIGH);
    }
    return;
  }

  // Builtin LED2 Switch
  if (strncmp(topic, MQTT_TOPIC_LED_2, strlen(MQTT_TOPIC_LED_2)) == 0) {
    if (strncmp((char*) payload, "ON", length) == 0) {
      digitalWrite(D4, LOW);
    } else if (strncmp((char*) payload, "OFF", length) == 0) {
      digitalWrite(D4, HIGH);
    }
    return;
  }

    
  // RC-Switch
  token = strtok((char*) topic, MQTT_TOPIC_DELIMITER);

  while (token != NULL) {
    if (segment == mqttBaseTopicSegmentCount) {
        strncpy(job.systemCode, token, 5);
    } else if (segment == mqttBaseTopicSegmentCount + 1) {
        strncpy(job.unitCode, token, 5);
    }

    // Bounds checking...
    if (segment > mqttBaseTopicSegmentCount + 1) {
      return;
    }

    token = strtok(NULL, MQTT_TOPIC_DELIMITER);
    segment++;
  }

  // Check for powersocket-commands
  if (isCodeValid(job.systemCode) && isCodeValid(job.unitCode)) {
    
    if (strncmp((char*) payload, "ON", length) == 0) {
      job.on = true;
    } else if (strncmp((char*) payload, "OFF", length) == 0) {
      job.on = false;
    } else {
      return;
    }
  
    rcJobQueue.push(job);
  }

  // Echo Topic has to be last job
  if ( echoTopic == 1 ){
    char buf[70];
    origTopic.toCharArray(buf, 70);
    mqttClient.publish(MQTT_TOPIC_ECHO, buf);
  }
  
}

void setup() {
  pinMode(BUTTON_PIN,INPUT_PULLUP);
  debouncer1.attach(BUTTON_PIN);
  debouncer1.interval(20); // interval in ms

  // Builtin LEDs
  pinMode(BUILTIN_LED, OUTPUT);
  pinMode(D4, OUTPUT);
  digitalWrite(D4, HIGH );
  
  // Serial + Power-Up Delays
  delay(500);
  Serial.begin(115200);
  delay(500);
  Serial.println("");

  
  rcSwitch.enableTransmit(D6);
  rcSwitch.setRepeatTransmit(RCSWITCH_TRANSMISSIONS);
  
  htu21.begin();

  // Count mqtt "segments" of base topic
  for (uint8_t i = 0; i < strlen(MQTT_TOPIC_RCSWITCH); i++) {
     if (MQTT_TOPIC_RCSWITCH[i] == MQTT_TOPIC_DELIMITER[0]) {
        mqttBaseTopicSegmentCount++;
     }
  }
  
  WiFi.hostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.print("WiFi connected to: ");
  Serial.println(WIFI_SSID);
  
  
  mqttClient.setClient(wifiClient);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  timer.setInterval(SENSOR_REFRESH_MS, []() {
    Serial.print("h: ");
    Serial.println(htu21.readHumidity());
    Serial.print("t: ");
    Serial.println(htu21.readTemperature());
    dtostrf(htu21.readHumidity(), 4, 2, convertBuffer);
    mqttClient.publish(MQTT_TOPIC_HUMIDITY, convertBuffer);

    dtostrf(htu21.readTemperature(), 4, 2, convertBuffer);
    mqttClient.publish(MQTT_TOPIC_TEMPERATURE, convertBuffer);
  });

  nextJobMillis = millis();
}
void loop() {
  mqttConnect();

  // handle button
  debouncer1.update();
  int buttonState1 = debouncer1.read();
  if ( buttonState1 == LOW) {
    if ( buttonStateLatest1 == false ){
      mqttClient.publish(MQTT_TOPIC_BUTTON, "ON");
      buttonStateLatest1 = true;
    }
  }else{
    if ( buttonStateLatest1 == true ){
      mqttClient.publish(MQTT_TOPIC_BUTTON, "OFF");
      buttonStateLatest1 = false;
    }
  }
  

  if (!rcJobQueue.empty() && millis() > nextJobMillis) {
    rcJob job = rcJobQueue.front();
    rcJobQueue.pop();

    noInterrupts();
    digitalWrite(BUILTIN_LED, LOW);
    
    if (job.on) {
      rcSwitch.switchOn(job.systemCode, job.unitCode);
    } else {
      rcSwitch.switchOff(job.systemCode, job.unitCode);
    }

    digitalWrite(BUILTIN_LED, HIGH);
    interrupts(); 
    
    nextJobMillis = millis() + RCSWITCH_PAUSE_MS;
  }

  timer.run();
  mqttClient.loop();
  ArduinoOTA.handle();
}
