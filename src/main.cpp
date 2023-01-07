/**
  ESP32 Flow sensor firmware for the Open eXtensible Rack System
  
  GitHub repository:
    https://github.com/sumnerboy12/OXRS-BJ-FlowSensor-ESP-FW
    
  Copyright 2023 Ben Jones <ben.jones12@gmail.com>
*/
/*--------------------------- Macros ----------------------------------*/
#define STRINGIFY(s) STRINGIFY1(s)
#define STRINGIFY1(s) #s

/*--------------------------- Libraries -------------------------------*/
#include <Arduino.h>
#include <ETH.h>                    // For networking
#include <SPI.h>                    // For ethernet
#include <WiFi.h>                   // For networking
#include <OXRS_MQTT.h>
#include <OXRS_API.h>
#include <MqttLogger.h>

/*--------------------------- Constants -------------------------------*/
// Serial
#define     SERIAL_BAUD_RATE                115200

// REST API
#define     REST_API_PORT                   80

// Sensor pin
#define     SENSOR_PIN                      2

// Config defaults and constraints
#define     DEFAULT_TELEMETRY_INTERVAL_MS   1000
#define     DEFAULT_K_FACTOR                49
#define     TELEMETRY_INTERVAL_MS_MAX       60000
#define     K_FACTOR_MAX                    1000

// Ethernet
#define     DHCP_TIMEOUT_MS                 15000
#define     DHCP_RESPONSE_TIMEOUT_MS        4000

#define     ETH_PHY_I2C_ADDR                1                     // I²C-address of Ethernet PHY (0 or 1 for LAN8720, 31 for TLK110)
#define     ETH_PHY_ENABLE                  16                    // Pin# of the enable signal for the external crystal oscillator (-1 to disable for internal APLL source)
#define     ETH_PHY_MDC                     23                    // Pin# of the I²C clock signal for the Ethernet PHY
#define     ETH_PHY_MDIO                    18                    // Pin# of the I²C IO signal for the Ethernet PHY
#define     ETH_PHY_TYPE                    ETH_PHY_LAN8720       // Type of the Ethernet PHY (LAN8720 or TLK110)  
#define     ETH_CLK_MODE                    ETH_CLOCK_GPIO0_IN    // Version with not PSRAM

/*--------------------------- Global Variables ------------------------*/
// Ethernet variables
bool ethernetConnected = false;

// Config variables
uint32_t telemetryIntervalMs = DEFAULT_TELEMETRY_INTERVAL_MS;
int kFactor = DEFAULT_K_FACTOR;

// Pulse count/telemetry variables
uint32_t pulseCount = 0L;
uint32_t lastTelemetryMs = 0L;
uint32_t elapsedTelemetryMs = 0L;

/*--------------------------- Instantiate Globals ---------------------*/
// Network client (for MQTT) and server (for REST API)
WiFiClient _client;
WiFiServer _server(REST_API_PORT);

// MQTT client
PubSubClient _mqttClient(_client);
OXRS_MQTT _mqtt(_mqttClient);

// REST API
OXRS_API _api(_mqtt);

// Logging
MqttLogger _logger(_mqttClient, "log", MqttLoggerMode::MqttAndSerial);

/*--------------------------- Program ---------------------------------*/
void IRAM_ATTR isr() {
  pulseCount++;
}

void getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json.createNestedObject("firmware");

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = STRINGIFY(FW_VERSION);

#if defined(FW_GITHUB_URL)
  firmware["githubUrl"] = FW_GITHUB_URL;
#endif
}

void getSystemJson(JsonVariant json)
{
  JsonObject system = json.createNestedObject("system");

  system["flashChipSizeBytes"] = ESP.getFlashChipSize();
  system["heapFreeBytes"] = ESP.getFreeHeap();
  system["heapUsedBytes"] = ESP.getHeapSize();
  system["heapMaxAllocBytes"] = ESP.getMaxAllocHeap();

  system["sketchSpaceUsedBytes"] = ESP.getSketchSize();
  system["sketchSpaceTotalBytes"] = ESP.getFreeSketchSpace();

  system["fileSystemUsedBytes"] = SPIFFS.usedBytes();
  system["fileSystemTotalBytes"] = SPIFFS.totalBytes();
}

void getNetworkJson(JsonVariant json)
{
  JsonObject network = json.createNestedObject("network");
  
  network["mode"] = "ethernet";
  network["ip"] = ETH.localIP();
  network["mac"] = ETH.macAddress();
}

void getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json.createNestedObject("configSchema");
  
  // Config schema metadata
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_SHORT_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema.createNestedObject("properties");

  JsonObject telemetryIntervalMs = properties.createNestedObject("telemetryIntervalMs");
  telemetryIntervalMs["title"] = "Telemetry Interval (ms)";
  telemetryIntervalMs["description"] = "How often to publish telemetry data (defaults to 1000ms, i.e. 1 second)";
  telemetryIntervalMs["type"] = "integer";
  telemetryIntervalMs["minimum"] = 1;
  telemetryIntervalMs["maximum"] = TELEMETRY_INTERVAL_MS_MAX;

  JsonObject kFactor = properties.createNestedObject("kFactor");
  kFactor["title"] = "K-Factor";
  kFactor["description"] = "Number of pulses per litre (defaults to 49, check flow sensor specs)";
  kFactor["type"] = "integer";
  kFactor["minimum"] = 1;
  kFactor["maximum"] = K_FACTOR_MAX;
}

void getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json.createNestedObject("commandSchema");
  
  // Command schema metadata
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_SHORT_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema.createNestedObject("properties");

  JsonObject restart = properties.createNestedObject("restart");
  restart["title"] = "Restart";
  restart["type"] = "boolean";
}

/**
  API callbacks
*/
void _apiAdopt(JsonVariant json)
{
  // Build device adoption info
  getFirmwareJson(json);
  getSystemJson(json);
  getNetworkJson(json);
  getConfigSchemaJson(json);
  getCommandSchemaJson(json);
}

/**
  MQTT callbacks
*/
void _mqttConnected()
{
  // MqttLogger doesn't copy the logging topic to an internal
  // buffer so we have to use a static array here
  static char logTopic[64];
  _logger.setTopic(_mqtt.getLogTopic(logTopic));

  // Publish device adoption info
  DynamicJsonDocument json(JSON_ADOPT_MAX_SIZE);
  _mqtt.publishAdopt(_api.getAdopt(json.as<JsonVariant>()));

  // Log the fact we are now connected
  _logger.println("[flow] mqtt connected");
}

void _mqttDisconnected(int state) 
{
  // Log the disconnect reason
  // See https://github.com/knolleary/pubsubclient/blob/2d228f2f862a95846c65a8518c79f48dfc8f188c/src/PubSubClient.h#L44
  switch (state)
  {
    case MQTT_CONNECTION_TIMEOUT:
      _logger.println(F("[flow] mqtt connection timeout"));
      break;
    case MQTT_CONNECTION_LOST:
      _logger.println(F("[flow] mqtt connection lost"));
      break;
    case MQTT_CONNECT_FAILED:
      _logger.println(F("[flow] mqtt connect failed"));
      break;
    case MQTT_DISCONNECTED:
      _logger.println(F("[flow] mqtt disconnected"));
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      _logger.println(F("[flow] mqtt bad protocol"));
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      _logger.println(F("[flow] mqtt bad client id"));
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      _logger.println(F("[flow] mqtt unavailable"));
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      _logger.println(F("[flow] mqtt bad credentials"));
      break;      
    case MQTT_CONNECT_UNAUTHORIZED:
      _logger.println(F("[flow] mqtt unauthorised"));
      break;      
  }
}

void _mqttCallback(char * topic, byte * payload, int length)
{
  // Pass down to our MQTT handler
  _mqtt.receive(topic, payload, length);
}

void _mqttConfig(JsonVariant json)
{
  if (json.containsKey("telemetryIntervalMs"))
  {
    telemetryIntervalMs = min(json["telemetryIntervalMs"].as<int>(), TELEMETRY_INTERVAL_MS_MAX);
  }

  if (json.containsKey("kFactor"))
  {
    kFactor = min(json["kFactor"].as<int>(), K_FACTOR_MAX);
  }
}

void _mqttCommand(JsonVariant json)
{
  if (json.containsKey("restart") && json["restart"].as<bool>())
  {
    ESP.restart();
  }
}

/**
  Initialisation
*/
void initialiseSerial()
{
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);
  
  _logger.println(F("[flow] starting up..."));

  DynamicJsonDocument json(128);
  getFirmwareJson(json.as<JsonVariant>());

  _logger.print(F("[flow] "));
  serializeJson(json, _logger);
  _logger.println();
}

void initialiseSensor(void)
{
  // Enable internal pullup on our sensor pin
  pinMode(SENSOR_PIN, INPUT_PULLUP);

  // Setup the sensor pin to trigger our interrupt service routine when 
  // pin goes from HIGH to LOW, i.e. FALLING edge
  attachInterrupt(SENSOR_PIN, isr, FALLING);
}

void initialiseMqtt(byte * mac)
{
  // Set the default client id to the last 3 bytes of the MAC address
  char clientId[32];
  sprintf_P(clientId, PSTR("%02x%02x%02x"), mac[3], mac[4], mac[5]);  
  _mqtt.setClientId(clientId);
  
  // Register our callbacks
  _mqtt.onConnected(_mqttConnected);
  _mqtt.onDisconnected(_mqttDisconnected);
  _mqtt.onConfig(_mqttConfig);
  _mqtt.onCommand(_mqttCommand);
  
  // Start listening for MQTT messages
  _mqttClient.setCallback(_mqttCallback);  
}

void initialiseRestApi(void)
{
  // NOTE: this must be called *after* initialising MQTT since that sets
  //       the default client id, which has lower precendence than MQTT
  //       settings stored in file and loaded by the API

  // Set up the REST API
  _api.begin();

  // Register our callbacks
  _api.onAdopt(_apiAdopt);

  // Start listening for API requests
  _server.begin();
}

void ethernetEvent(WiFiEvent_t event)
{
  // Log the event to serial for debugging
  switch (event)
  {
    case ARDUINO_EVENT_ETH_START:
      // Get the ethernet MAC address
      byte mac[6];
      ETH.macAddress(mac);

      // Display the MAC address on serial
      char mac_display[18];
      sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      _logger.print(F("[flow] mac address: "));
      _logger.println(mac_display);

      // Set up MQTT (don't attempt to connect yet)
      initialiseMqtt(mac);
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      _logger.println(F("[flow] ethernet connected"));
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      // Display the IP address assigned by DHCP on serial
      _logger.print(F("[flow] ip address: "));
      _logger.println(ETH.localIP());

      ethernetConnected = true;
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      _logger.println(F("[flow] ethernet disconnected"));

      ethernetConnected = false;
      break;

    case ARDUINO_EVENT_ETH_STOP:
      _logger.println(F("[flow] ethernet stopped"));

      ethernetConnected = false;
      break;
  }
}

void initialiseNetwork()
{
  // We continue initialisation inside this event handler
  WiFi.onEvent(ethernetEvent);

  // Start the Ethernet PHY and wait for events
  ETH.begin(ETH_PHY_I2C_ADDR, ETH_PHY_ENABLE, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE);
}

/**
  Setup
*/
void setup() 
{
  // Set up serial
  initialiseSerial();  

  // Set up sensor
  initialiseSensor();

  // Set up network
  initialiseNetwork();

  // Set up REST API
  initialiseRestApi();
}

/**
  Main processing loop
*/
void loop() 
{
  // Wait until our ethernet connection is up
  if (!ethernetConnected)
  {
    delay(100);
    return;
  }

  // Check our MQTT broker connection is still ok
  _mqtt.loop();

  // Handle any API requests
  WiFiClient client = _server.available();
  _api.loop(&client);

  // Check if we need to send telemetry
  elapsedTelemetryMs = millis() - lastTelemetryMs;
  if (elapsedTelemetryMs >= telemetryIntervalMs)
  {
    // Build telemetry payload
    StaticJsonDocument<128> json;
    json["elapsedMs"] = elapsedTelemetryMs;
    json["pulseCount"] = pulseCount;
    json["volumeMls"] = (uint32_t)(pulseCount * 1000 / kFactor);
    
    // Publish telemetry and reset loop variables if successful
    if (_mqtt.publishTelemetry(json))
    {
      lastTelemetryMs = millis();
      pulseCount = 0;
    }
  }
}
