/**
  ESP8266 Flow sensor firmware for the Open eXtensible Rack System
  
  GitHub repository:
    https://github.com/sumnerboy12/OXRS-BJ-FlowSensor-ESP-FW
    
  Copyright 2023 Ben Jones <ben.jones12@gmail.com>
*/

/*--------------------------- Libraries -------------------------------*/
#include <Arduino.h>
#include <OXRS_Room8266.h>

/*--------------------------- Constants -------------------------------*/
// Serial
#define   SERIAL_BAUD_RATE                115200

// Sensor pin
#define   SENSOR_PIN                      4       // SDA on IDC header

// Config defaults and constraints
#define   DEFAULT_TELEMETRY_INTERVAL_MS   1000
#define   DEFAULT_K_FACTOR                49
#define   TELEMETRY_INTERVAL_MS_MAX       60000
#define   K_FACTOR_MAX                    1000

/*--------------------------- Global Variables ------------------------*/
// Config variables
uint32_t  telemetryIntervalMs           = DEFAULT_TELEMETRY_INTERVAL_MS;
int       kFactor                       = DEFAULT_K_FACTOR;

// Pulse count/telemetry variables
uint32_t  pulseCount                    = 0L;
uint32_t  lastTelemetryMs               = 0L;
uint32_t  elapsedTelemetryMs            = 0L;

/*--------------------------- Instantiate Globals ---------------------*/
// Room8266 handler
OXRS_Room8266 room8266;

/*--------------------------- Program ---------------------------------*/
void IRAM_ATTR isr() 
{
  pulseCount++;
}

void setConfigSchema()
{
  // Define our config schema
  StaticJsonDocument<1024> json;
  
  JsonObject telemetryIntervalMs = json.createNestedObject("telemetryIntervalMs");
  telemetryIntervalMs["title"] = "Telemetry Interval (ms)";
  telemetryIntervalMs["description"] = "How often to publish telemetry data (defaults to 1000ms, i.e. 1 second)";
  telemetryIntervalMs["type"] = "integer";
  telemetryIntervalMs["minimum"] = 1;
  telemetryIntervalMs["maximum"] = TELEMETRY_INTERVAL_MS_MAX;

  JsonObject kFactor = json.createNestedObject("kFactor");
  kFactor["title"] = "K-Factor";
  kFactor["description"] = "Number of pulses per litre (defaults to 49, check flow sensor specs)";
  kFactor["type"] = "integer";
  kFactor["minimum"] = 1;
  kFactor["maximum"] = K_FACTOR_MAX;

  // Pass our config schema down to the Room8266 library
  room8266.setConfigSchema(json.as<JsonVariant>());
}

void jsonConfig(JsonVariant json)
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

/**
  Setup
*/
void setup() 
{
  // Start serial and let settle
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);
  Serial.println(F("[flow] starting up..."));

  // Enable internal pullup on our sensor pin
  pinMode(SENSOR_PIN, INPUT_PULLUP);

  // Setup the sensor pin to trigger our interrupt service routine when 
  // pin goes from HIGH to LOW, i.e. FALLING edge
  attachInterrupt(SENSOR_PIN, isr, FALLING);

  // Log the pin we are monitoring for pulse events
  room8266.print(F("[flow] pulse sensor pin: "));
  room8266.println(SENSOR_PIN);

  // Start Room8266 hardware
  room8266.begin(jsonConfig, NULL);

  // Set up config schema (for self-discovery and adoption)
  setConfigSchema();
}

/**
  Main processing loop
*/
void loop() 
{
  // Let Room8266 hardware handle any events etc
  room8266.loop();

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
    if (room8266.publishTelemetry(json))
    {
      lastTelemetryMs = millis();
      pulseCount = 0;
    }
  }
}
