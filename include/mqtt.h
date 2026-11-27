#include <PubSubClient.h>
#include <EEPROM.h>
#include "restart.h"
#include "comm.h"

#define MQTT_attr "espaltherma/ATTR"
#define MQTT_lwt "espaltherma/LWT"

#define EEPROM_CHK 1
#define EEPROM_STATE 0

#define MQTT_attr "espaltherma/ATTR"
#define MQTT_lwt "espaltherma/LWT"

#ifdef JSONTABLE
char jsonbuff[MAX_MSG_SIZE] = "[{\0";
#else
char jsonbuff[MAX_MSG_SIZE] = "{\0";
#endif

#ifdef MQTT_ENCRYPTED
#include <WiFiClientSecure.h>
WiFiClientSecure espClient;
#else
WiFiClient espClient;
#endif
PubSubClient client(espClient);

#ifndef ARDUINO_ARCH_ESP8266
#include <esp_task_wdt.h>
#endif

// The discovery configs are retained on the broker, so they only need to reach it once per boot.
// Re-sending ~6 KB on every reconnect over a -90 dBm link kept the session busy past the broker's
// keepalive limit, so it was dropped mid-publish and the ESP looped reconnect -> discovery -> drop.
static bool discoverySent = false;

/*
 * Publishes a retained device discovery profile to MQTT, so Home Assistant can auto-configure our device and its sensors.
 */
void publishHomeAssistantDeviceDiscovery();

void sendValues()
{
  mqttSerial.printf("Sending values in MQTT.\n");
#ifdef ARDUINO_M5Stick_C_Plus2
  //Add Power values
  // getBatteryVoltage returns battery voltage [mV] as an int16_t
  float batteryVoltage = (float) M5.Power.getBatteryVoltage() / 1000; // convert to V as a float
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%.3gV\",", "M5BatV", batteryVoltage);
#elif ARDUINO_M5Stick_C
  //Add M5 APX values
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%.3gV\",\"%s\":\"%gmA\",", "M5VIN", M5.Power.Axp192.getVBUSVoltage(),"M5AmpIn", M5.Power.Axp192.getVBUSCurrent());
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%.3gV\",\"%s\":\"%gmA\",", "M5BatV", M5.Power.Axp192.getBatteryVoltage(),"M5BatCur", M5.Power.Axp192.getBatteryChargeCurrent() - M5.Power.Axp192.getBatteryDischargeCurrent());
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%.3gmW\",", "M5BatPwr", M5.Power.Axp192.getBatteryPower());
#elif ARDUINO_M5Stick_S3
  //Add M5 Stick-S3 Power values
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%.3gV\",", "M5VIN", ((float)M5.Power.getVBUSVoltage())/1000);
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%.3gV\",", "M5BatV", ((float)M5.Power.getBatteryVoltage())/1000);
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%d%%\",", "M5BatLvl", M5.Power.getBatteryLevel());
#endif
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%ddBm\",", "WifiRSSI", WiFi.RSSI());
  snprintf(jsonbuff + strlen(jsonbuff),MAX_MSG_SIZE - strlen(jsonbuff) , "\"%s\":\"%d\",", "FreeMem", ESP.getFreeHeap());
  jsonbuff[strlen(jsonbuff) - 1] = '}';
#ifdef JSONTABLE
  strcat(jsonbuff,"]");
#endif
  client.publish(MQTT_attr, jsonbuff);
#ifdef JSONTABLE
  strcpy(jsonbuff, "[{\0");
#else
  strcpy(jsonbuff, "{\0");
#endif
}

void saveEEPROM(uint8_t state){
    EEPROM.write(EEPROM_STATE,state);
    EEPROM.commit();
}

void readEEPROM(){
  if ('R' == EEPROM.read(EEPROM_CHK)){
    digitalWrite(PIN_THERM,EEPROM.read(EEPROM_STATE));
    mqttSerial.printf("Restoring previous state: %s",(EEPROM.read(EEPROM_STATE) == PIN_THERM_ACTIVE_STATE)? "On":"Off" );
  }
  else{
    mqttSerial.printf("EEPROM not initialized (%d). Initializing...",EEPROM.read(EEPROM_CHK));
    EEPROM.write(EEPROM_CHK,'R');
    EEPROM.write(EEPROM_STATE,!PIN_THERM_ACTIVE_STATE);
    EEPROM.commit();
    digitalWrite(PIN_THERM,!PIN_THERM_ACTIVE_STATE);
  }
}

// Defined in main.cpp; used to keep the screen/button responsive and WiFi
// recovering while we wait between MQTT connection attempts.
void handleScreen();
void checkWifi();
bool wifiLinkUp();                       // real link state (see wifimgr.h)
void wifiForceReconnect(const char *why);
void wifiDiagPublish(PubSubClient &mqtt); // outage stats -> espaltherma/diag
#define MQTT_FAILS_BEFORE_WIFI_RESET 12     // ~1 min of failures on a link that looks up
#define MQTT_REBOOT_AFTER_MS (20UL * 60UL * 1000UL)

void reconnectMqtt()
{
  // Loop until we're reconnected
  int i = 0;
  unsigned long firstTry = millis();
  while (!client.connected())
  {
    if (!wifiLinkUp())
    { // No point retrying MQTT without WiFi; recover it first
      checkWifi();
      firstTry = millis();//WiFi has its own reboot timer; restart ours
      i = 0;
    }
    ledSet(LED_MQTT);
    mqttSerial.print("Attempting MQTT connection...");

    if (client.connect("ESPAltherma-dev", MQTT_USERNAME, MQTT_PASSWORD, MQTT_lwt, 0, true, "Offline"))
    {
      mqttSerial.println("connected!");
      if (!discoverySent)
      {
      client.publish("homeassistant/sensor/espAltherma/config", "{\"name\":\"AlthermaSensors\",\"stat_t\":\"~/LWT\",\"avty_t\":\"~/LWT\",\"pl_avail\":\"Online\",\"pl_not_avail\":\"Offline\",\"uniq_id\":\"espaltherma\",\"device\":{\"identifiers\":[\"ESPAltherma\"]}, \"~\":\"espaltherma\",\"json_attr_t\":\"~/ATTR\"}", true);
      }
      client.publish(MQTT_lwt, "Online", true);
      wifiDiagPublish(client);
      if (!discoverySent)
      {
      client.publish("homeassistant/switch/espAltherma/config", "{\"name\":\"Altherma\",\"cmd_t\":\"~/POWER\",\"stat_t\":\"~/STATE\",\"pl_off\":\"OFF\",\"pl_on\":\"ON\",\"~\":\"espaltherma\"}", true);

      publishHomeAssistantDeviceDiscovery();
      }

      // Subscribe
      client.subscribe("espaltherma/POWER");
#ifdef PIN_SG1
      // Smart Grid
      client.publish("homeassistant/select/espAltherma/sg/config", "{\"availability\":[{\"topic\":\"espaltherma/LWT\",\"payload_available\":\"Online\",\"payload_not_available\":\"Offline\"}],\"availability_mode\":\"all\",\"unique_id\":\"espaltherma_sg\",\"device\":{\"identifiers\":[\"ESPAltherma\"],\"manufacturer\":\"ESPAltherma\",\"model\":\"M5StickC PLUS ESP32-PICO\",\"name\":\"ESPAltherma\"},\"icon\":\"mdi:solar-power\",\"name\":\"EspAltherma Smart Grid\",\"command_topic\":\"espaltherma/sg/set\",\"command_template\":\"{% if value == 'Free Running' %} 0 {% elif value == 'Forced Off' %} 1 {% elif value == 'Recommended On' %} 2 {% elif value == 'Forced On' %} 3 {% else %} 0 {% endif %}\",\"options\":[\"Free Running\",\"Forced Off\",\"Recommended On\",\"Forced On\"],\"state_topic\":\"espaltherma/sg/state\",\"value_template\":\"{% set mapper = { '0':'Free Running', '1':'Forced Off', '2':'Recommended On', '3':'Forced On' } %} {% set word = mapper[value] %} {{ word }}\"}", true);
      client.subscribe("espaltherma/sg/set");
      client.publish("espaltherma/sg/state", "0");
#endif

#ifdef SAFETY_RELAY_PIN
      // Safety relay
      client.publish("homeassistant/switch/espAltherma/safety/config", "{\"name\":\"Altherma Safety\",\"cmd_t\":\"~/SAFETY\",\"stat_t\":\"~/SAFETY_STATE\",\"pl_off\":\"0\",\"pl_on\":\"1\",\"~\":\"espaltherma\"}", true);
      client.subscribe("espaltherma/SAFETY");
#endif

#ifdef DEBUG_SERIAL
      // DebugSerial - MQTT<>Serial gateway
      client.subscribe("espaltherma/serialTX");
#endif

#ifndef PIN_SG1
      // Publish empty retained message so discovered entities are removed from HA
      client.publish("homeassistant/select/espAltherma/sg/config", "", true);
#endif
    }
    else
    {
      mqttSerial.printf("failed, rc=%d, try again in 5 seconds", client.state());
      unsigned long start = millis();
      while (millis() < start + 5000)
      {
        ArduinoOTA.handle();
        handleScreen();//Keep the button responsive while retrying
        delay(10);
      }

      i++;
      if (i % MQTT_FAILS_BEFORE_WIFI_RESET == 0)
      { //The link may only look up (stale status, dead association): reconnect WiFi
        wifiForceReconnect("MQTT keeps failing");
      }
      if (millis() - firstTry >= MQTT_REBOOT_AFTER_MS) {
        mqttSerial.printf("No MQTT for 20 min, rebooting now.");
        restart_board();
      }
    }
  }
}

void callbackTherm(byte *payload, unsigned int length)
{
  payload[length] = '\0';

  // Is it ON or OFF?
  // Ok I'm not super proud of this, but it works :p
  if (payload[1] == 'F')
  { //turn off
    digitalWrite(PIN_THERM, !PIN_THERM_ACTIVE_STATE);
    saveEEPROM(!PIN_THERM_ACTIVE_STATE);
    client.publish("espaltherma/STATE", "OFF", true);
    mqttSerial.println("Turned OFF");
  }
  else if (payload[1] == 'N')
  { //turn on
    digitalWrite(PIN_THERM, PIN_THERM_ACTIVE_STATE);
    saveEEPROM(PIN_THERM_ACTIVE_STATE);
    client.publish("espaltherma/STATE", "ON", true);
    mqttSerial.println("Turned ON");
  }
  else if (payload[0] == 'R')//R(eset/eboot)
  {
    mqttSerial.println("Rebooting");
    delay(100);
    restart_board();
  }
  else
  {
    mqttSerial.printf("Unknown message: %s\n", payload);
  }
}

#ifdef PIN_SG1
//Smartgrid callbacks
void callbackSg(byte *payload, unsigned int length)
{
  payload[length] = '\0';

  if (payload[0] == '0')
  {
    // Set SG 0 mode => SG1 = INACTIVE, SG2 = INACTIVE
    digitalWrite(PIN_SG1, SG_RELAY_INACTIVE_STATE);
    digitalWrite(PIN_SG2, SG_RELAY_INACTIVE_STATE);
    client.publish("espaltherma/sg/state", "0");
    mqttSerial.println("Set SG mode to 0 - Normal operation");
  }
  else if (payload[0] == '1')
  {
    // Set SG 1 mode => SG1 = INACTIVE, SG2 = ACTIVE
    digitalWrite(PIN_SG1, SG_RELAY_INACTIVE_STATE);
    digitalWrite(PIN_SG2, SG_RELAY_ACTIVE_STATE);
    client.publish("espaltherma/sg/state", "1");
    mqttSerial.println("Set SG mode to 1 - Forced OFF");
  }
  else if (payload[0] == '2')
  {
    // Set SG 2 mode => SG1 = ACTIVE, SG2 = INACTIVE
    digitalWrite(PIN_SG1, SG_RELAY_ACTIVE_STATE);
    digitalWrite(PIN_SG2, SG_RELAY_INACTIVE_STATE);
    client.publish("espaltherma/sg/state", "2");
    mqttSerial.println("Set SG mode to 2 - Recommended ON");
  }
  else if (payload[0] == '3')
  {
    // Set SG 3 mode => SG1 = ACTIVE, SG2 = ACTIVE
    digitalWrite(PIN_SG1, SG_RELAY_ACTIVE_STATE);
    digitalWrite(PIN_SG2, SG_RELAY_ACTIVE_STATE);
    client.publish("espaltherma/sg/state", "3");
    mqttSerial.println("Set SG mode to 3 - Forced ON");
  }
  else
  {
    mqttSerial.printf("Unknown message: %s\n", payload);
  }
}
#endif

#ifdef SAFETY_RELAY_PIN
void callbackSafety(byte *payload, unsigned int length)
{
  payload[length] = '\0';

  if (payload[0] == '0')
  {
    // Set Safety relay to OFF
    digitalWrite(SAFETY_RELAY_PIN, !SAFETY_RELAY_ACTIVE_STATE);
    client.publish("espaltherma/SAFETY_STATE", "0", true);
  }
  else if (payload[0] == '1')
  {
    // Set Safety relay to ON
    digitalWrite(SAFETY_RELAY_PIN, SAFETY_RELAY_ACTIVE_STATE);
    client.publish("espaltherma/SAFETY_STATE", "1", true);
  }
  else
  {
    mqttSerial.printf("Unknown message: %s\n", payload);
  }
}
#endif

#ifdef DEBUG_SERIAL
void callbackDebugSerial(byte *payload, unsigned int length)
{
  payload[length] = '\0';
  
  // Send message to serial port
  MySerial.write(payload, length);
  MySerial.flush();
  
  // Wait for response with timeout
  unsigned long startTime = millis();
  const unsigned long timeout = 1000; // 1 second timeout
  byte responseBuffer[256]; // Buffer to store raw binary data
  int responseLength = 0;
  
  while (millis() - startTime < timeout && responseLength < sizeof(responseBuffer))
  {
    if (MySerial.available())
    {
      responseBuffer[responseLength++] = MySerial.read();
      
      // Reset timeout if we're still receiving data
      startTime = millis();
    }
    
    // Allow other operations during waiting
    client.loop();
    ArduinoOTA.handle();
    yield();
  }
  
  // Publish response if we got any data, encoded as hex string
  if (responseLength > 0)
  {
    String hexResponse = "";
    for (int i = 0; i < responseLength; i++)
    {
      if (i > 0) hexResponse += " ";
      if (responseBuffer[i] < 0x10) hexResponse += "0";
      hexResponse += String(responseBuffer[i], HEX);
    }
    hexResponse.toUpperCase();
    
    client.publish("espaltherma/serialRX", hexResponse.c_str());
  }
}
#endif


void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.printf("Message arrived [%s] : %.*s\n", topic, (int)length, (const char *)payload);

  if (strcmp(topic, "espaltherma/POWER") == 0)
  {
    callbackTherm(payload, length);
  }
#ifdef PIN_SG1
  else if (strcmp(topic, "espaltherma/sg/set") == 0)
  {
    callbackSg(payload, length);
  }
#endif
#ifdef SAFETY_RELAY_PIN
  else if (strcmp(topic, "espaltherma/SAFETY") == 0)
  {
    callbackSafety(payload, length);
  }
#endif
#ifdef DEBUG_SERIAL
  else if (strcmp(topic, "espaltherma/serialTX") == 0)
  {
    callbackDebugSerial(payload, length);
  }
#endif

  else
  {
    mqttSerial.printf("Unknown topic: %s\n", topic);
  }
}

// Streams a device-discovery payload chunk straight to the MQTT socket (used as a DiscoverySink).
struct DiscoveryBuf
{
  char *data;
  size_t len;
};

static void mqttDiscoverySink(void *ctx, const char *data, size_t len)
{
  DiscoveryBuf *b = static_cast<DiscoveryBuf *>(ctx);
  memcpy(b->data + b->len, data, len);
  b->len += len;
}

void publishHomeAssistantDeviceDiscovery()
{
  const size_t count = sizeof(labelDefs) / sizeof(LabelDef);
  const char *topic = "homeassistant/device/espaltherma-mqtt-discovery/config";

  // First pass: compute the exact payload length, required up-front for the MQTT fixed header.
  size_t payloadLen = streamDeviceDiscoveryPayload(labelDefs, count, nullptr, nullptr);

  mqttSerial.printf("Sending HA discovery: %u sensors, %u bytes\n",
                    (unsigned)(count + 3), (unsigned)payloadLen);

  // Render into one heap buffer and hand it to the socket in a single write, so it goes out as a
  // few full TCP segments instead of hundreds of tiny ones (each tiny write waited for the lossy link).
  DiscoveryBuf b = {static_cast<char *>(malloc(payloadLen)), 0};
  if (!b.data)
  {
    mqttSerial.println("No heap for device discovery, will retry on the next connect");
    return;
  }
  streamDeviceDiscoveryPayload(labelDefs, count, mqttDiscoverySink, &b);

#ifndef ARDUINO_ARCH_ESP8266
  esp_task_wdt_reset();
#endif
  bool ok = client.beginPublish(topic, payloadLen, true) &&
            client.write(reinterpret_cast<const uint8_t *>(b.data), b.len) == b.len &&
            client.endPublish();
  free(b.data);
#ifndef ARDUINO_ARCH_ESP8266
  esp_task_wdt_reset();
#endif

  if (ok && client.connected())
  {
    discoverySent = true;
    mqttSerial.println("HA discovery sent");
  }
  else
  {
    mqttSerial.println("HA discovery failed, will retry on the next connect");
  }
}
