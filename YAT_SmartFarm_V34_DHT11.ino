/****************************************************
 YAT SMART FARM V3.4 REAL PRODUCTION
 Controller : ESP8266 NodeMCU

 Hardware
 ----------
 RTC DS3231
 Relay 4CH
  D5 Pump
  D6 Zone1
  D7 Zone2
  D8 Light

 Sensor : NONE
****************************************************/

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson
#include <ESPAsyncWebServer.h> // https://github.com/me-no-dev/ESPAsyncWebServer
#include <LittleFS.h> // For ESP8266 file system
#include <DHT.h>

// ===== DHT11 SENSOR =====
#define DHTPIN D4
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);
float temperature = NAN;
float humidity = NAN;
bool dhtAvailable = false;
unsigned long lastDHTRead = 0;
const unsigned long DHT_READ_INTERVAL_MS = 2000;


#include <Wire.h>
#include <RTClib.h>
#include <ArduinoOTA.h>
#include <Ticker.h> // For watchdog timer
#include <UniversalTelegramBot.h> // https://github.com/witnessmenow/UniversalTelegramBot

#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h> // For HTTPS requests to OpenWeatherMap and Telegram


RTC_DS3231 rtc;

WiFiClient espClient;
WiFiClientSecure telegramClient;
UniversalTelegramBot bot(BOT_TOKEN, telegramClient);


// Define AsyncWebServer
AsyncWebServer server(80);

// Flag for saving data
bool shouldSaveConfig = false;

// Callback notifying us of the need to save config
void saveConfigCallback() {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}


//==================== WIFI ====================




//==================== RELAY ====================

#define RELAY_PUMP   D5
#define RELAY_ZONE1  D6
#define RELAY_ZONE2  D7
#define RELAY_LIGHT  D8

//==================== STATE ====================

bool pumpState=false;
bool zone1State=false;
bool zone2State=false;
bool lightState=false;

bool autoMode=true;

// Soil Moisture Sensor
#define SOIL_MOISTURE_PIN A0
int soilMoistureValue = 0;

// AI Auto Watering variables
unsigned long lastWateringTime = 0;
unsigned long wateringCooldown = 6 * 60 * 60 * 1000; // 6 hours cooldown
int soilMoistureThreshold = 500; // Adjustable threshold
int wateringDurationAI = 5; // Adjustable watering duration in minutes

String aiStatus = "Idle";
String lastAIDecision = "N/A";

// User Management variables
struct User {
  String email;
  String password; // Hashed in production
  String role; // "admin", "manager", "operator", "viewer"
  bool active;
};

#define MAX_USERS 10
User users[MAX_USERS];
int userCount = 0;

String currentUserEmail = "";
String currentUserRole = "";




// Countdown Timer variables
unsigned long timerStartTime = 0;
unsigned int timerDuration = 0; // in minutes
bool timerRunning = false;

// Log System variables
struct LogEntry {
  unsigned long timestamp;
  String message;
};

#define MAX_LOGS 1000
LogEntry logs[MAX_LOGS];
int logCount = 0;
int logHead = 0; // Points to the next available slot

void sendTelegramMessage(String message) {
  if (BOT_TOKEN == "8667185180:AAEaPMQFRUW7AhqgSFdMgMdzzZTAY4OIbj" || CHAT_ID == "8698930095"
   
  ") {
    Serial.println("Telegram Bot Token or Chat ID not set. Skipping Telegram message.");
    return;
  }
  telegramClient.setInsecure(); // Use with caution, for development only
  bot.sendMessage(CHAT_ID, message, "");
  addLog("Telegram: " + message);
}

void addLog(String message) {
  logs[logHead].timestamp = millis();
  logs[logHead].message = message;
  logHead = (logHead + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) {
    logCount++;
  }
  Serial.print("LOG: ");
  Serial.println(message);
  // Send Telegram message for important events
  if (message.startsWith("WiFi Connected") ||
      message.startsWith("WiFi Lost") ||
      message.startsWith("OTA Start") ||
      message.startsWith("OTA End") ||
      message.startsWith("OTA Error") ||
      message.startsWith("Pump set to") ||
      message.startsWith("Zone1 set to") ||
      message.startsWith("Zone2 set to") ||
      message.startsWith("AI: Watering started") ||
      message.startsWith("AI: Watering finished") ||
      message.startsWith("Countdown timer finished.")) {
    sendTelegramMessage(message);
  }
}

unsigned long lastStatus=0;
unsigned long lastReconnect=0;
//==================== WIFI ====================



//==================== RELAY ====================

void setRelay(uint8_t pin, bool state) {
  String relayName;
  if (pin == RELAY_PUMP) relayName = "Pump";
  else if (pin == RELAY_ZONE1) relayName = "Zone1";
  else if (pin == RELAY_ZONE2) relayName = "Zone2";
  else if (pin == RELAY_LIGHT) relayName = "Light";
  addLog(relayName + " set to " + (state ? "ON" : "OFF"));

  digitalWrite(pin, state ? LOW : HIGH);

  switch (pin) {

    case RELAY_PUMP:
      pumpState = state;
      break;

    case RELAY_ZONE1:
      zone1State = state;
      break;

    case RELAY_ZONE2:
      zone2State = state;
      break;

    case RELAY_LIGHT:
      lightState = state;
      break;

  }

}

void relayInit() {

  pinMode(RELAY_PUMP, OUTPUT);
  pinMode(RELAY_ZONE1, OUTPUT);
  pinMode(RELAY_ZONE2, OUTPUT);
  pinMode(RELAY_LIGHT, OUTPUT);

  setRelay(RELAY_PUMP, false);
  setRelay(RELAY_ZONE1, false);
  setRelay(RELAY_ZONE2, false);
  setRelay(RELAY_LIGHT, false);

}

void relayAllOff() {

  setRelay(RELAY_PUMP, false);
  setRelay(RELAY_ZONE1, false);
  setRelay(RELAY_ZONE2, false);
  setRelay(RELAY_LIGHT, false);

}

void startCountdownTimer(unsigned int duration) {
  if (timerRunning) {
    Serial.println("Timer already running. Cannot start multiple timers.");
    return;
  }
  timerDuration = duration;
  timerStartTime = millis();
  timerRunning = true;
  setRelay(RELAY_PUMP, true); // Start pump when timer starts
  Serial.printf("Countdown timer started for %u minutes.\n", timerDuration);
}

void stopCountdownTimer() {
  timerRunning = false;
  timerDuration = 0;
  timerStartTime = 0;
  setRelay(RELAY_PUMP, false); // Stop pump when timer stops
  Serial.println("Countdown timer stopped.");
}

unsigned int getRemainingTimerMinutes() {
  if (!timerRunning) return 0;
  unsigned long elapsedMillis = millis() - timerStartTime;
  unsigned int elapsedMinutes = elapsedMillis / (1000 * 60);
  if (elapsedMinutes >= timerDuration) {
    stopCountdownTimer();
    return 0;
  }
  return timerDuration - elapsedMinutes;
}

void aiAutoWatering() {
  // Check if AI watering is enabled and not in manual mode
  if (!autoMode) {
    aiStatus = "Disabled (Manual Mode)";
    return;
  }

  // Check if pump is already running from timer
  if (timerRunning) {
    aiStatus = "Pump busy (Timer running)";
    return;
  }

  // Cooldown period after last watering
  if (millis() - lastWateringTime < wateringCooldown) {
    aiStatus = "Cooldown period";
    return;
  }

  // Get current time from RTC
  DateTime now = rtc.now();
  int currentHour = now.hour();

  // Check watering time windows (05:00-09:00 and 16:00-18:00)
  bool isWateringWindow = (currentHour >= 5 && currentHour < 9) || (currentHour >= 16 && currentHour < 18);
  if (!isWateringWindow) {
    aiStatus = "Outside watering window";
    return;
  }

  // Fetch weather data if it's stale (e.g., older than 10 minutes)
  if (millis() - weatherData.lastUpdated > 10 * 60 * 1000) {
    fetchWeatherData();
  }

  // Never water if Rain Probability > 70% OR Rain > 1 mm
  if (weatherData.rainProbability > 0.7 || weatherData.rain1h > 1.0) {
    aiStatus = "Rain predicted or currently raining";
    lastAIDecision = "No watering (Rain)";
    addLog("AI: No watering due to rain prediction.");
    return;
  }

  // Check soil moisture threshold
  if (soilMoistureValue > soilMoistureThreshold) { // Assuming higher value means drier soil
    aiStatus = "Soil moisture sufficient";
    lastAIDecision = "No watering (Soil moisture)";
    addLog("AI: No watering, soil moisture sufficient.");
    return;
  }

  // If all conditions met, start watering
  if (!pumpState) { // Only start if pump is not already on
    setRelay(RELAY_PUMP, true);
    lastWateringTime = millis();
    aiStatus = "Watering";
    lastAIDecision = "Watering started for " + String(wateringDurationAI) + " minutes";
    addLog("AI: Watering started for " + String(wateringDurationAI) + " minutes.");
    // Implement auto-stop after wateringDurationAI minutes (can use a timer or check in loop)
    // For now, we'll just set the pump state and let the main loop handle the duration if needed.
    // A more robust solution would involve a separate timer for AI watering.
  }

  // Stop watering after duration (simple implementation, can be improved with a dedicated timer)
  if (pumpState && (millis() - lastWateringTime > (unsigned long)wateringDurationAI * 60 * 1000)) {
    setRelay(RELAY_PUMP, false);
    aiStatus = "Idle";
    lastAIDecision = "Watering finished";
    addLog("AI: Watering finished.");
  }
}

void handleTelegramMessages() {
  if (BOT_TOKEN == "YOUR_TELEGRAM_BOT_TOKEN" || CHAT_ID == "YOUR_TELEGRAM_CHAT_ID") {
    return;
  }
  int numNewMessages = bot.getUpdates(bot.lastUpdateID + 1);

  while (numNewMessages) {
    Serial.println("got response");
    for (int i = 0; i < numNewMessages; i++) {
      String chat_id = String(bot.messages[i].chat_id);
      String text = bot.messages[i].text;

      if (chat_id == CHAT_ID) { // Only process messages from authorized chat ID
        Serial.println("Received Telegram message: " + text);
        addLog("Telegram Command: " + text);

        if (text == "/status") {
          String statusMsg = "*Current Status*\n";
          statusMsg += "Pump: " + String(pumpState ? "ON" : "OFF") + "\n";
          statusMsg += "Zone1: " + String(zone1State ? "ON" : "OFF") + "\n";
          statusMsg += "Zone2: " + String(zone2State ? "ON" : "OFF") + "\n";
          statusMsg += "Light: " + String(lightState ? "ON" : "OFF") + "\n";
          statusMsg += "Mode: " + String(autoMode ? "Auto" : "Manual") + "\n";
          statusMsg += "IP: " + WiFi.localIP().toString() + "\n";
          if (timerRunning) {
            statusMsg += "Timer: Running, " + String(getRemainingTimerMinutes()) + " min left\n";
          } else {
            statusMsg += "Timer: Idle\n";
          }
          statusMsg += "AI Status: " + aiStatus + "\n";
          statusMsg += "Last AI Decision: " + lastAIDecision + "\n";
          sendTelegramMessage(statusMsg);
        } else if (text == "/help") {
          String helpMsg = "*Available Commands*\n";
          helpMsg += "/status - Get current system status\n";
          helpMsg += "/help - Show this help message\n";
          helpMsg += "/pump on - Turn pump ON\n";
          helpMsg += "/pump off - Turn pump OFF\n";
          helpMsg += "/zone1 on - Turn Zone 1 ON\n";
          helpMsg += "/zone1 off - Turn Zone 1 OFF\n";
          helpMsg += "/zone2 on - Turn Zone 2 ON\n";
          helpMsg += "/zone2 off - Turn Zone 2 OFF\n";
          helpMsg += "/reboot - Reboot the ESP8266\n";
          helpMsg += "/log - Get latest system logs\n";
          helpMsg += "/weather - Get current weather information\n";
          sendTelegramMessage(helpMsg);
        } else if (text == "/pump on") {
          setRelay(RELAY_PUMP, true);
          sendTelegramMessage("Pump turned ON");
        } else if (text == "/pump off") {
          setRelay(RELAY_PUMP, false);
          sendTelegramMessage("Pump turned OFF");
        } else if (text == "/zone1 on") {
          setRelay(RELAY_ZONE1, true);
          sendTelegramMessage("Zone 1 turned ON");
        } else if (text == "/zone1 off") {
          setRelay(RELAY_ZONE1, false);
          sendTelegramMessage("Zone 1 turned OFF");
        } else if (text == "/zone2 on") {
          setRelay(RELAY_ZONE2, true);
          sendTelegramMessage("Zone 2 turned ON");
        } else if (text == "/zone2 off") {
          setRelay(RELAY_ZONE2, false);
          sendTelegramMessage("Zone 2 turned OFF");
        } else if (text == "/reboot") {
          sendTelegramMessage("Rebooting device...");
          ESP.restart();
        } else if (text == "/log") {
          String logMsg = "*Latest Logs*\n";
          for (int j = 0; j < logCount; j++) {
            int idx = (logHead + j) % MAX_LOGS;
            logMsg += String(logs[idx].timestamp) + ": " + logs[idx].message + "\n";
          }
          sendTelegramMessage(logMsg);
        } else if (text == "/weather") {
          fetchWeatherData();
          String weatherMsg = "*Current Weather*\n";
          weatherMsg += "Location: " + WEATHER_CITY + ", " + WEATHER_COUNTRY + "\n";
          weatherMsg += "Temperature: " + String(weatherData.temperature) + "°C\n";
          weatherMsg += "Humidity: " + String(weatherData.humidity) + "%\n";
          weatherMsg += "Description: " + weatherData.description + "\n";
          weatherMsg += "Wind Speed: " + String(weatherData.windSpeed) + " m/s\n";
          weatherMsg += "Rain (1h): " + String(weatherData.rain1h) + " mm\n";
          weatherMsg += "Rain Prob: " + String(weatherData.rainProbability * 100) + "%\n";
          sendTelegramMessage(weatherMsg);
        } else {
          sendTelegramMessage("Unknown command. Type /help for a list of commands.");
        }
      } else {
        Serial.println("Unauthorized chat ID: " + chat_id);
        bot.sendMessage(chat_id, "Unauthorized access.", "");
      }
    }
    numNewMessages = bot.getUpdates(bot.lastUpdateID + 1);
  }
}

void handleCountdownTimer() {
  if (timerRunning && getRemainingTimerMinutes() == 0) {
    Serial.println("Countdown timer finished.");
    // Add notification for timer finished later
  }
}

void publishStatus() {
  addLog("Status Update: Pump=" + String(pumpState) + ", Zone1=" + String(zone1State) + ", Zone2=" + String(zone2State) + ", Light=" + String(lightState) + ", Mode=" + (autoMode ? "auto" : "manual"));

  StaticJsonDocument<256> doc;

  doc["pump"] = pumpState;
  doc["zone1"] = zone1State;
  doc["zone2"] = zone2State;
  doc["light"] = lightState;
  doc["mode"] = autoMode ? "auto" : "manual";
  doc["ip"] = WiFi.localIP().toString();

  String payload;
  serializeJson(doc, payload);

  

}
//==================== OTA ====================

void otaInit() {

  ArduinoOTA.setHostname("YAT-SMART-FARM");

  ArduinoOTA.onStart([]() {

    relayAllOff();

    Serial.println("OTA START");
    addLog("OTA Start");

  });

  ArduinoOTA.onEnd([]() {

    Serial.println("OTA END");
    addLog("OTA End");

  });

  ArduinoOTA.onProgress([](unsigned int progress,
                           unsigned int total) {

    Serial.printf("OTA %u%%\n",
                  (progress * 100) / total);

  });

  ArduinoOTA.onError([](ota_error_t error) {

    Serial.printf("OTA ERROR %u\n", error);
    addLog("OTA Error: " + String(error));

  });

  ArduinoOTA.begin();

}

//==================== SETUP ====================

void initRestAPI() {
  // Serve index.html
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
    } else {
      // Fallback if file doesn't exist
      String html = "<!DOCTYPE html><html><body><h1>SMART FARM PRO ULTIMATE V35</h1><p>Dashboard not available. Please upload index.html to the device.</p></body></html>";
      request->send(200, "text/html", html);
    }
  });

  // GET /api/status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["pump"] = pumpState;
    doc["zone1"] = zone1State;
    doc["zone2"] = zone2State;
    doc["light"] = lightState;
    doc["mode"] = autoMode ? "auto" : "manual";
    doc["ip"] = WiFi.localIP().toString();
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // GET /api/sensors
  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["soil_moisture"] = soilMoistureValue;
    if (dhtAvailable) {
      doc["temperature"] = temperature;
      doc["humidity"] = humidity;
    } else {
      doc["temperature"] = nullptr;
      doc["humidity"] = nullptr;
    }
    doc["dht11_available"] = dhtAvailable;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // GET /api/relays
  server.on("/api/relays", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["pump"] = pumpState;
    doc["zone1"] = zone1State;
    doc["zone2"] = zone2State;
    doc["light"] = lightState;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // GET /api/timer
  server.on("/api/timer", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<128> doc;
    doc["running"] = timerRunning;
    doc["remaining_minutes"] = getRemainingTimerMinutes();
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // GET /api/logs
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<2048> doc;
    JsonArray logArray = doc.to<JsonArray>();
    for (int i = 0; i < logCount; i++) {
      JsonObject logEntry = logArray.add<JsonObject>();
      logEntry["timestamp"] = logs[(logHead + i) % MAX_LOGS].timestamp;
      logEntry["message"] = logs[(logHead + i) % MAX_LOGS].message;
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // GET /api/weather (Placeholder)
  server.on("/api/weather", HTTP_GET, [](AsyncWebServerRequest *request) {
    StaticJsonDocument<256> doc;
    doc["message"] = "Weather data will be implemented in Phase 2";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // GET /api/system
  server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest *request) {
    String response = deviceStatus();
    request->send(200, "application/json", response);
  });

  // POST /api/pump
  server.on("/api/pump", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<128> doc;
      DeserializationError error = deserializeJson(doc, (char*)data);
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (doc.containsKey("state")) {
        setRelay(RELAY_PUMP, doc["state"]);
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing 'state' parameter\"}");
      }
  });

  // POST /api/zone
  server.on("/api/zone", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<128> doc;
      DeserializationError error = deserializeJson(doc, (char*)data);
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (doc.containsKey("zone") && doc.containsKey("state")) {
        int zone = doc["zone"];
        bool state = doc["state"];
        if (zone == 1) {
          setRelay(RELAY_ZONE1, state);
        } else if (zone == 2) {
          setRelay(RELAY_ZONE2, state);
        } else {
          request->send(400, "application/json", "{\"error\":\"Invalid zone\"}");
          return;
        }
        request->send(200, "application/json", "{\"status\":\"ok\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing 'zone' or 'state' parameter\"}");
      }
  });

  // POST /api/timer
  server.on("/api/timer", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<128> doc;
      DeserializationError error = deserializeJson(doc, (char*)data);
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (doc.containsKey("action")) {
        String action = doc["action"].as<String>();
        if (action == "start" && doc.containsKey("duration")) {
          unsigned int duration = doc["duration"];
          if (duration >= 1 && duration <= 120) {
            startCountdownTimer(duration);
            request->send(200, "application/json", "{\"status\":\"timer started\"}");
          } else {
            request->send(400, "application/json", "{\"error\":\"Duration must be between 1 and 120 minutes\"}");
          }
        } else if (action == "stop") {
          stopCountdownTimer();
          request->send(200, "application/json", "{\"status\":\"timer stopped\"}");
        } else {
          request->send(400, "application/json", "{\"error\":\"Invalid timer action or missing duration\"}");
        }
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing action parameter\"}");
      }
  });

  // POST /api/settings (Placeholder)
  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<128> doc;
      DeserializationError error = deserializeJson(doc, (char*)data);
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      doc["message"] = "Settings control will be implemented in Phase 3";
      String response;
      serializeJson(doc, response);
      request->send(200, "application/json", response);
  });

  // POST /api/reboot
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"status\":\"rebooting\"}");
    delay(100);
    ESP.restart();
  });

  // POST /api/resetwifi
  server.on("/api/resetwifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    request->send(200, "application/json", "{\"status\":\"WiFi settings reset, rebooting\"}");
    delay(100);
    ESP.restart();
  });

  // POST /api/login
  server.on("/api/login", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<128> doc;
      DeserializationError error = deserializeJson(doc, (char*)data);
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (doc.containsKey("email") && doc.containsKey("password")) {
        String email = doc["email"].as<String>();
        String password = doc["password"].as<String>();
        if (authenticateUser(email, password)) {
          request->send(200, "application/json", "{\"status\":\"authenticated\",\"role\":\"" + currentUserRole + "\"}");
        } else {
          request->send(401, "application/json", "{\"error\":\"Invalid credentials\"}");
        }
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing email or password\"}");
      }
  });

  // GET /api/users (Admin only)
  server.on("/api/users", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkPermission("manage_users")) {
      request->send(403, "application/json", "{\"error\":\"Forbidden\"}");
      return;
    }
    StaticJsonDocument<2048> doc;
    JsonArray usersArray = doc.to<JsonArray>();
    for (int i = 0; i < userCount; i++) {
      JsonObject userObj = usersArray.add<JsonObject>();
      userObj["email"] = users[i].email;
      userObj["role"] = users[i].role;
      userObj["active"] = users[i].active;
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // Start server
  server.begin();
}

Ticker watchdogTimer;

void resetWatchdog() {
  ESP.wdtFeed();
}

bool authenticateUser(String email, String password) {
  for (int i = 0; i < userCount; i++) {
    if (users[i].email == email && users[i].password == password && users[i].active) {
      currentUserEmail = email;
      currentUserRole = users[i].role;
      addLog("User authenticated: " + email + " (" + users[i].role + ")");
      return true;
    }
  }
  addLog("Authentication failed for: " + email);
  return false;
}

bool checkPermission(String action) {
  if (currentUserRole == "admin") return true; // Admin can do everything
  if (currentUserRole == "manager") {
    if (action == "operate" || action == "manage_users" || action == "view_logs") return true;
  }
  if (currentUserRole == "operator") {
    if (action == "operate") return true;
  }
  if (currentUserRole == "viewer") {
    if (action == "view") return true;
  }
  return false;
}

void readSoilMoisture() {
  soilMoistureValue = analogRead(SOIL_MOISTURE_PIN);
}


// ===== DHT11 READING =====
void readDHT11() {
  const unsigned long now = millis();
  if (now - lastDHTRead < DHT_READ_INTERVAL_MS) return;
  lastDHTRead = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    dhtAvailable = false;
    Serial.println(F("DHT11 Read Failed"));
    return;
  }

  dhtAvailable = true;
  temperature = t;
  humidity = h;

  Serial.print(F("Temperature: "));
  Serial.print(temperature, 1);
  Serial.println(F(" C"));
  Serial.print(F("Humidity: "));
  Serial.print(humidity, 1);
  Serial.println(F(" %"));

  if (mqtt.connected()) {
    char value[8];
    dtostrf(temperature, 0, 1, value);
    mqtt.publish("smartfarm/temperature", value, true);
    dtostrf(humidity, 0, 1, value);
    mqtt.publish("smartfarm/humidity", value, true);
  }
}


// Returns DHT11 values as a JSON object fragment.
// Use this inside the existing /api/sensors response to expose:
// "temperature": <value>, "humidity": <value>, "available": true/false
String dht11Json() {
  if (!dhtAvailable) {
    return String("\"temperature\":null,\"humidity\":null,\"available\":false");
  }
  String json = "\"temperature\":" + String(temperature, 1);
  json += ",\"humidity\":" + String(humidity, 1);
  json += ",\"available\":true";
  return json;
}

void setup() {
  dht.begin();


  Serial.begin(115200);

  if(!LittleFS.begin()){
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }

  relayInit();

  rtcInit();





  // WiFiManager
  WiFiManager wifiManager;

  // Set config save callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // Set static IP
  // wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));

  // Fetches ssid and pass from eeprom and tries to connect
  // If it does not connect it starts an access point with the specified name
  // And goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect("AutoConnectAP", "password")) {
    Serial.println("Failed to connect and hit timeout");
    ESP.reset();
    delay(1000);
  }

  Serial.println("WiFi connected");
  addLog("WiFi Connected: " + WiFi.localIP().toString());
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  otaInit();

  publishStatus();

  initRestAPI();

  // Initialize watchdog timer
  watchdogTimer.attach(10, resetWatchdog); // Feed watchdog every 10 seconds

}

//==================== LOOP ====================

void loop() {
  readDHT11();


  ArduinoOTA.handle();
  // Feed watchdog in loop
  ESP.wdtFeed();
  readSoilMoisture();





  autoControl();
  handleCountdownTimer();
  aiAutoWatering();
  handleTelegramMessages();

  if (millis() - lastStatus >= 5000) {

    lastStatus = millis();

    publishStatus();

  }

}
//==================== JSON CONFIG ====================

#include <ArduinoJson.h>


//==================== FIREBASE PLACEHOLDER ====================
// ใส่ค่า Firebase จริงภายหลัง

const char* FIREBASE_HOST = "";
const char* FIREBASE_AUTH = "";

// Telegram Bot
const String BOT_TOKEN = "YOUR_TELEGRAM_BOT_TOKEN";
const String CHAT_ID = "YOUR_TELEGRAM_CHAT_ID";

// OpenWeatherMap API
const String OPENWEATHERMAP_API_KEY = "YOUR_OPENWEATHERMAP_API_KEY"; // Replace with your actual API key
const String WEATHER_CITY = "Bangkok";
const String WEATHER_COUNTRY = "TH";

// Weather data structure
struct WeatherData {
  float temperature;
  int humidity;
  int pressure;
  float windSpeed;
  int cloudiness;
  float rain1h;
  float rainProbability;
  unsigned long sunrise;
  unsigned long sunset;
  String description;
  unsigned long lastUpdated;
} weatherData;



//==================== SEND DATA ====================

void sendFirebaseStatus(){

  // ส่งสถานะ Relay
  // Firebase RTDB integration

  Serial.println("Firebase Update");

}


//==================== DEVICE INFO ====================

String deviceStatus(){

  StaticJsonDocument<256> doc;

  doc["device"] = "YAT-FARM-001";
  doc["firmware"] = "V3.4";
  doc["time"] = getDateTime();

  String result;

  serializeJson(doc,result);

  return result;

}


//==================== HEARTBEAT ====================

void heartbeat(){

  StaticJsonDocument<128> doc;

  doc["online"] = true;
  doc["device"] = "YAT-FARM-001";

  String data;

  serializeJson(doc,data);



}
//==================== FIREBASE DATA STRUCTURE ====================

void updateCloudStatus();
void fetchWeatherData() {
  if (OPENWEATHERMAP_API_KEY == "YOUR_OPENWEATHERMAP_API_KEY") {
    Serial.println("OpenWeatherMap API Key not set. Skipping weather data fetch.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Use with caution, for development only

  HTTPClient http;
  String url = "https://api.openweathermap.org/data/2.5/weather?q=" + WEATHER_CITY + "," + WEATHER_COUNTRY + "&APPID=" + OPENWEATHERMAP_API_KEY + "&units=metric";
  Serial.println("Fetching weather from: " + url);

  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        addLog("Weather API Error: JSON parsing failed");
        return;
      }

      weatherData.temperature = doc["main"]["temp"];
      weatherData.humidity = doc["main"]["humidity"];
      weatherData.pressure = doc["main"]["pressure"];
      weatherData.windSpeed = doc["wind"]["speed"];
      weatherData.cloudiness = doc["clouds"]["all"];
      weatherData.description = doc["weather"][0]["description"].as<String>();
      weatherData.sunrise = doc["sys"]["sunrise"];
      weatherData.sunset = doc["sys"]["sunset"];
      weatherData.rain1h = doc["rain"]["1h"] | 0.0; // Default to 0.0 if not present
      weatherData.rainProbability = doc["pop"] | 0.0; // Probability of precipitation, if available
      weatherData.lastUpdated = millis();

      Serial.println("Weather data fetched successfully.");
      addLog("Weather data updated");
    }
  } else {
    Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    addLog("Weather API Error: " + String(http.errorToString(httpCode).c_str()));
  }

  http.end();
}

void updateCloudStatus(){

  StaticJsonDocument<256> doc;

  doc["deviceID"] = "YAT-FARM-001";

  doc["relay"]["pump"]  = pumpState;
  doc["relay"]["zone1"] = zone1State;
  doc["relay"]["zone2"] = zone2State;
  doc["relay"]["light"] = lightState;

  doc["mode"] = autoMode ? "AUTO" : "MANUAL";

  doc["rtc"] = getDateTime();

  String json;

  serializeJson(doc,json);

  Serial.println(json);

}


//==================== COMMAND PROCESS ====================

void processCommand(String command){

  StaticJsonDocument<256> doc;

  DeserializationError error =
      deserializeJson(doc,command);


  if(error){

    Serial.println("JSON ERROR");

    return;

  }


  if(doc.containsKey("pump")){

    setRelay(
      RELAY_PUMP,
      doc["pump"]
    );

  }


  if(doc.containsKey("zone1")){

    setRelay(
      RELAY_ZONE1,
      doc["zone1"]
    );

  }


  if(doc.containsKey("zone2")){

    setRelay(
      RELAY_ZONE2,
      doc["zone2"]
    );

  }


  if(doc.containsKey("light")){

    setRelay(
      RELAY_LIGHT,
      doc["light"]
    );

  }


  if(doc.containsKey("auto")){

    autoMode = doc["auto"];

  }


  publishStatus();

}
//==================== NETWORK CHECK ====================

void checkWiFi(){

  if(WiFi.status()!=WL_CONNECTED){

    Serial.println("WiFi Lost");

    WiFi.disconnect();

  

  }

}


//==================== DEVICE RESTART ====================

void restartDevice(){

  relayAllOff();

  delay(1000);

  ESP.restart();

}


//==================== SAFETY SYSTEM ====================

void safetyCheck(){

  // ป้องกัน Relay ค้าง

  if(!autoMode){

    return;

  }


  // ถ้ามีการควบคุมจาก Manual
  // จะไม่ให้ Timer แทรก

}


//==================== DEBUG ====================

void debugStatus(){

  Serial.println("================");

  Serial.print("Pump : ");
  Serial.println(pumpState);

  Serial.print("Zone1 : ");
  Serial.println(zone1State);

  Serial.print("Zone2 : ");
  Serial.println(zone2State);

  Serial.print("Light : ");
  Serial.println(lightState);

  Serial.print("Mode : ");

  Serial.println(
    autoMode ? "AUTO":"MANUAL"
  );

  Serial.print("RTC : ");

  Serial.println(
    getDateTime()
  );

  Serial.println("================");

}
//==================== MQTT SETUP ====================

void mqttInit(){

  mqtt.setServer(
    MQTT_SERVER,
    MQTT_PORT
  );

  mqtt.setCallback(
    mqttCallback
  );

}


//==================== CONNECTION MANAGER ====================

void connectionManager(){



  if(!mqtt.connected()){

  

  }



}


//==================== SYSTEM LOOP TASK ====================

void systemTask(){

  connectionManager();

  safetyCheck();


  if(millis() - lastStatus > 5000){

    lastStatus = millis();

    publishStatus();

    updateCloudStatus();

    heartbeat();

  }

}


//==================== MANUAL CONTROL API ====================

void manualPump(bool state){

  autoMode = false;

  setRelay(
    RELAY_PUMP,
    state
  );

}


void manualZone1(bool state){

  autoMode = false;

  setRelay(
    RELAY_ZONE1,
    state
  );

}


void manualZone2(bool state){

  autoMode = false;

  setRelay(
    RELAY_ZONE2,
    state
  );

}


void manualLight(bool state){

  autoMode = false;

  setRelay(
    RELAY_LIGHT,
    state
  );

}
//==================== TIMER CONTROL ====================

struct TimerConfig {

  bool enable;

  uint8_t startHour;
  uint8_t startMinute;

  uint8_t stopHour;
  uint8_t stopMinute;

};


TimerConfig zone1Timer = {

  true,
  6,
  0,
  6,
  30

};


TimerConfig zone2Timer = {

  true,
  17,
  0,
  17,
  30

};


TimerConfig lightTimer = {

  true,
  18,
  0,
  22,
  0

};


//==================== TIMER CHECK ====================

bool checkTimer(TimerConfig timer){

  DateTime now = rtc.now();


  int current =
    now.hour() * 60 +
    now.minute();


  int start =
    timer.startHour * 60 +
    timer.startMinute;


  int stop =
    timer.stopHour * 60 +
    timer.stopMinute;


  if(start <= current && current < stop){

    return true;

  }


  return false;

}


//==================== AUTO ZONE CONTROL ====================

void autoZoneControl(){

  if(!autoMode){

    return;

  }


  setRelay(
    RELAY_ZONE1,
    checkTimer(zone1Timer)
  );


  setRelay(
    RELAY_ZONE2,
    checkTimer(zone2Timer)
  );


  setRelay(
    RELAY_LIGHT,
    checkTimer(lightTimer)
  );


}


//==================== MAIN CONTROL ====================

void runController(){

  autoControl();

  autoZoneControl();

  systemTask();

}
//==================== FIREBASE PATH ====================

String firebasePath(){

  String path = "/device/";

  path += "YAT-FARM-001";

  return path;

}


//==================== CLOUD PAYLOAD ====================

String createCloudPayload(){

  StaticJsonDocument<512> doc;


  doc["device"]["id"] =
    "YAT-FARM-001";


  doc["device"]["firmware"] =
    "V3.4";


  doc["device"]["online"] =
    true;


  doc["device"]["time"] =
    getDateTime();



  doc["control"]["mode"] =
    autoMode ? "AUTO":"MANUAL";


  doc["control"]["pump"] =
    pumpState;


  doc["control"]["zone1"] =
    zone1State;


  doc["control"]["zone2"] =
    zone2State;


  doc["control"]["light"] =
    lightState;



  String output;


  serializeJson(
    doc,
    output
  );


  return output;

}


//==================== SEND MQTT STATUS ====================

void sendMQTTStatus(){

  String data =
    createCloudPayload();





}


//==================== SYSTEM START MESSAGE ====================

void systemReady(){

  Serial.println();
  Serial.println("====================");
  Serial.println("YAT SMART FARM V3.4");
  Serial.println("SYSTEM READY");
  Serial.println("====================");

}
//==================== SETUP FINAL UPDATE ====================

void setupSystem(){

  Serial.begin(115200);

  delay(1000);


  systemReady();


  // Relay

  relayInit();


  // RTC

  rtcInit();


  // WiFi




  // MQTT

  mqttInit();


  // OTA

  otaInit();



  publishStatus();


  sendMQTTStatus();


}


//==================== LOOP TASK ====================

void loopSystem(){


  ArduinoOTA.handle();


  runController();


  if(millis() - lastStatus > 10000){

    lastStatus = millis();


    debugStatus();


    sendMQTTStatus();

  }


}


//==================== MAIN LOOP EXTEND ====================

// ใช้แทน loop เดิม

void loop(){

  loopSystem();

}
//==================== POWER SAVE / WATCHDOG ====================

void watchdogInit(){

  ESP.wdtEnable(8000);

}


void watchdogFeed(){

  ESP.wdtFeed();

}


//==================== MEMORY CHECK ====================

void memoryStatus(){

  Serial.print("Free Heap : ");

  Serial.println(
    ESP.getFreeHeap()
  );

}


//==================== DEVICE HEALTH ====================

void deviceHealth(){

  StaticJsonDocument<256> doc;


  doc["device"] =
    "YAT-FARM-001";


  doc["firmware"] =
    "V3.4";


  doc["heap"] =
    ESP.getFreeHeap();


  doc["uptime"] =
    millis() / 1000;


  String data;


  serializeJson(
    doc,
    data
  );




}


//==================== RECOVERY ====================

void systemRecovery(){

  if(WiFi.status()
     != WL_CONNECTED){

  

  }


  if(!mqtt.connected()){

  

  }


  watchdogFeed();

}
//==================== FIREBASE CONFIG ====================

struct FirebaseConfig {

  String apiKey;

  String databaseURL;

};


FirebaseConfig firebase;


//==================== LOAD CONFIG ====================

void loadConfig(){

  firebase.apiKey =
    "YOUR_FIREBASE_API_KEY";


  firebase.databaseURL =
    "YOUR_DATABASE_URL";


}


//==================== CLOUD CONTROL RECEIVE ====================

void cloudControl(String json){

  StaticJsonDocument<256> doc;


  if(deserializeJson(doc,json)){

    return;

  }


  if(doc.containsKey("mode")){

    String mode =
      doc["mode"].as<String>();


    if(mode=="AUTO"){

      autoMode=true;

    }
    else{

      autoMode=false;

    }

  }


  if(doc.containsKey("pump")){

    setRelay(
      RELAY_PUMP,
      doc["pump"]
    );

  }


  if(doc.containsKey("zone1")){

    setRelay(
      RELAY_ZONE1,
      doc["zone1"]
    );

  }


  if(doc.containsKey("zone2")){

    setRelay(
      RELAY_ZONE2,
      doc["zone2"]
    );

  }


  if(doc.containsKey("light")){

    setRelay(
      RELAY_LIGHT,
      doc["light"]
    );

  }


  publishStatus();

}


//==================== SYSTEM SERVICE ====================

void serviceTask(){

  watchdogFeed();

  systemRecovery();

  memoryStatus();

  deviceHealth();

}
//==================== COMMAND QUEUE ====================

String commandBuffer = "";

void receiveCommand(){

  while(mqtt.available()){

    char c = mqtt.read();

    commandBuffer += c;

  }


  if(commandBuffer.length()){

    processCommand(commandBuffer);

    commandBuffer = "";

  }

}


//==================== RELAY STATUS JSON ====================

String relayStatus(){

  StaticJsonDocument<256> doc;


  doc["pump"] =
    pumpState;


  doc["zone1"] =
    zone1State;


  doc["zone2"] =
    zone2State;


  doc["light"] =
    lightState;


  doc["mode"] =
    autoMode ? "AUTO":"MANUAL";

  if (dhtAvailable) {
    doc["sensors"]["temperature"] = temperature;
    doc["sensors"]["humidity"] = humidity;
  }


  String output;


  serializeJson(
    doc,
    output
  );


  return output;

}


//==================== PUBLISH RELAY STATUS ====================

void publishRelayStatus(){

  String data =
    relayStatus();




}


//==================== SCHEDULE RESET ====================

void resetSchedule(){

  zone1Timer.enable = false;

  zone2Timer.enable = false;

  lightTimer.enable = false;


  Serial.println(
    "Schedule Reset"
  );

}


//==================== FACTORY SAFE ====================

void factorySafe(){

  relayAllOff();

  autoMode = false;

  Serial.println(
    "SYSTEM SAFE MODE"
  );

}
//==================== STARTUP CHECK ====================

void startupCheck(){

  Serial.println();
  Serial.println("STARTUP CHECK");


  if(WiFi.status()==WL_CONNECTED){

    Serial.println("WiFi OK");

  }
  else{

    Serial.println("WiFi FAIL");

  }


  if(rtc.begin()){

    Serial.println("RTC OK");

  }
  else{

    Serial.println("RTC FAIL");

  }


  Serial.println("Relay Ready");

}


//==================== DEVICE INFORMATION ====================

String deviceInfo(){

  StaticJsonDocument<256> doc;


  doc["id"] =
    "YAT-FARM-001";


  doc["version"] =
    "3.4.0";


  doc["controller"] =
    "ESP8266";


  doc["rtc"] =
    getDateTime();


  String result;


  serializeJson(
    doc,
    result
  );


  return result;

}


//==================== SEND DEVICE INFO ====================

void publishDeviceInfo(){

  String data =
    deviceInfo();




}


//==================== END SYSTEM SERVICE ====================

void finalService(){

  watchdogFeed();


  if(millis()%30000==0){

    publishDeviceInfo();

  }

}
//==================== MQTT TOPIC HANDLER ====================

void handleMQTTCommand(String message){

  StaticJsonDocument<512> doc;

  DeserializationError error =
    deserializeJson(doc, message);


  if(error){

    Serial.println("Command JSON Error");

    return;

  }


  if(doc.containsKey("pump")){

    setRelay(
      RELAY_PUMP,
      doc["pump"]
    );

  }


  if(doc.containsKey("zone1")){

    setRelay(
      RELAY_ZONE1,
      doc["zone1"]
    );

  }


  if(doc.containsKey("zone2")){

    setRelay(
      RELAY_ZONE2,
      doc["zone2"]
    );

  }


  if(doc.containsKey("light")){

    setRelay(
      RELAY_LIGHT,
      doc["light"]
    );

  }


  if(doc.containsKey("mode")){

    String mode =
      doc["mode"].as<String>();


    autoMode =
      (mode == "AUTO");

  }


  publishRelayStatus();

}


//==================== MQTT MESSAGE LOG ====================

void logCommand(String msg){

  Serial.print("MQTT RX : ");

  Serial.println(msg);

}


//==================== SYSTEM LOOP EXTENSION ====================

void productionLoop(){

  ArduinoOTA.handle();


  connectionManager();


  autoZoneControl();


  serviceTask();


  finalService();


}
//==================== MQTT TOPIC HANDLER ====================

void handleMQTTCommand(String message){

  StaticJsonDocument<512> doc;

  DeserializationError error =
    deserializeJson(doc, message);


  if(error){

    Serial.println("Command JSON Error");

    return;

  }


  if(doc.containsKey("pump")){

    setRelay(
      RELAY_PUMP,
      doc["pump"]
    );

  }


  if(doc.containsKey("zone1")){

    setRelay(
      RELAY_ZONE1,
      doc["zone1"]
    );

  }


  if(doc.containsKey("zone2")){

    setRelay(
      RELAY_ZONE2,
      doc["zone2"]
    );

  }


  if(doc.containsKey("light")){

    setRelay(
      RELAY_LIGHT,
      doc["light"]
    );

  }


  if(doc.containsKey("mode")){

    String mode =
      doc["mode"].as<String>();


    if(mode == "AUTO"){

      autoMode = true;

    }
    else{

      autoMode = false;

    }

  }


  publishRelayStatus();

}


//==================== COMMAND LOG ====================

void logCommand(String msg){

  Serial.print("MQTT RX : ");

  Serial.println(msg);

}


//==================== PRODUCTION LOOP ====================

void productionLoop(){

  ArduinoOTA.handle();

  connectionManager();

  autoZoneControl();

  serviceTask();

  finalService();

}
//==================== ERROR HANDLING ====================

void systemError(String error){

  Serial.print("SYSTEM ERROR : ");

  Serial.println(error);


  StaticJsonDocument<128> doc;


  doc["device"] =
    "YAT-FARM-001";


  doc["error"] =
    error;


  String data;


  serializeJson(
    doc,
    data
  );


  mqtt.publish(
    TOPIC_STATUS,
    data.c_str()
  );

}


//==================== RELAY TEST ====================

void relayTest(){

  Serial.println("Relay Test Start");


  setRelay(RELAY_PUMP,true);
  delay(1000);

  setRelay(RELAY_PUMP,false);


  setRelay(RELAY_ZONE1,true);
  delay(1000);

  setRelay(RELAY_ZONE1,false);


  setRelay(RELAY_ZONE2,true);
  delay(1000);

  setRelay(RELAY_ZONE2,false);


  setRelay(RELAY_LIGHT,true);
  delay(1000);

  setRelay(RELAY_LIGHT,false);


  Serial.println("Relay Test Complete");

}


//==================== REMOTE RESTART ====================

void remoteRestart(){

  relayAllOff();

  delay(500);

  ESP.restart();

}


//==================== VERSION ====================

String firmwareVersion(){

  return "YAT-SmartFarm-V3.4";
}
//==================== COMMAND ACTION ====================

void executeSystemCommand(String cmd){

  if(cmd == "SAFE"){

    factorySafe();

  }


  else if(cmd == "RESTART"){

    remoteRestart();

  }


  else if(cmd == "TEST_RELAY"){

    relayTest();

  }


  else if(cmd == "STATUS"){

    publishStatus();

  }


  else{

    Serial.println("Unknown Command");

  }

}


//==================== DEVICE STATE ====================

String deviceState(){

  StaticJsonDocument<256> doc;


  doc["device"] =
    "YAT-FARM-001";


  doc["firmware"] =
    firmwareVersion();


  doc["pump"] =
    pumpState;


  doc["zone1"] =
    zone1State;


  doc["zone2"] =
    zone2State;


  doc["light"] =
    lightState;


  doc["mode"] =
    autoMode ? "AUTO":"MANUAL";


  if (dhtAvailable) {
    doc["sensors"]["temperature"] = temperature;
    doc["sensors"]["humidity"] = humidity;
  }


  String output;


  serializeJson(
    doc,
    output
  );


  return output;

}


//==================== STATUS UPDATE ====================

void sendDeviceState(){

  String state =
    deviceState();


  mqtt.publish(
    TOPIC_STATUS,
    state.c_str(),
    true
  );
}
//==================== EEPROM CONFIG ====================

#include <EEPROM.h>

#define EEPROM_SIZE 64


struct DeviceConfig {

  bool autoMode;

  bool pump;
  bool zone1;
  bool zone2;
  bool light;

};


DeviceConfig config;


//==================== SAVE CONFIG ====================

void saveConfig(){

  config.autoMode = autoMode;

  config.pump = pumpState;
  config.zone1 = zone1State;
  config.zone2 = zone2State;
  config.light = lightState;


  EEPROM.put(
    0,
    config
  );


  EEPROM.commit();


}


//==================== LOAD CONFIG ====================

void loadConfig(){

  EEPROM.begin(
    EEPROM_SIZE
  );


  EEPROM.get(
    0,
    config
  );


  autoMode =
    config.autoMode;


  pumpState =
    config.pump;


  zone1State =
    config.zone1;


  zone2State =
    config.zone2;


  lightState =
    config.light;


}


//==================== SAFE BOOT ====================

void safeBoot(){

  relayAllOff();

  loadConfig();


  Serial.println(
    "SAFE BOOT COMPLETE"
  );

}


//==================== CONFIG UPDATE ====================

void updateConfig(){

  saveConfig();

  publishStatus();

}
//==================== SCHEDULE CLOUD FORMAT ====================

struct CloudSchedule {

  bool enable;

  uint8_t startHour;

  uint8_t startMinute;

  uint8_t stopHour;

  uint8_t stopMinute;

};


CloudSchedule pumpCloud;


//==================== LOAD DEFAULT SCHEDULE ====================

void defaultSchedule(){

  pumpCloud.enable = true;

  pumpCloud.startHour = 6;

  pumpCloud.startMinute = 0;

  pumpCloud.stopHour = 6;

  pumpCloud.stopMinute = 30;

}


//==================== CHECK PUMP SCHEDULE ====================

bool pumpScheduleCheck(){

  if(!pumpCloud.enable){

    return false;

  }


  DateTime now =
    rtc.now();


  int current =
    now.hour()*60 +
    now.minute();


  int start =
    pumpCloud.startHour*60 +
    pumpCloud.startMinute;


  int stop =
    pumpCloud.stopHour*60 +
    pumpCloud.stopMinute;


  return
    current >= start &&
    current < stop;

}


//==================== AUTO PUMP ====================

void autoPumpControl(){

  if(!autoMode){

    return;

  }


  if(pumpScheduleCheck()){

    setRelay(
      RELAY_PUMP,
      true
    );

  }
  else{

    setRelay(
      RELAY_PUMP,
      false
    );

  }

}
//==================== CONFIG MQTT UPDATE ====================

void updateScheduleFromMQTT(String data){

  StaticJsonDocument<256> doc;


  if(deserializeJson(doc,data)){

    return;

  }


  if(doc.containsKey("enable")){

    pumpCloud.enable =
      doc["enable"];

  }


  if(doc.containsKey("startHour")){

    pumpCloud.startHour =
      doc["startHour"];

  }


  if(doc.containsKey("startMinute")){

    pumpCloud.startMinute =
      doc["startMinute"];

  }


  if(doc.containsKey("stopHour")){

    pumpCloud.stopHour =
      doc["stopHour"];

  }


  if(doc.containsKey("stopMinute")){

    pumpCloud.stopMinute =
      doc["stopMinute"];

  }


  saveConfig();

}


//==================== CLOUD COMMAND ROUTER ====================

void commandRouter(String command){

  if(command.startsWith("SCHEDULE")){

    updateScheduleFromMQTT(
      command.substring(8)
    );

  }

  else if(command.startsWith("{")){

    handleMQTTCommand(command);

  }

  else{

    executeSystemCommand(command);

  }

}


//==================== SYSTEM RUNNER ====================

void runProduction(){

  connectionManager();

  autoPumpControl();

  autoZoneControl();

  productionLoop();

}
//==================== FINAL SETUP ====================

void setup(){

  Serial.begin(115200);

  delay(1000);


  Serial.println();

  Serial.println(
    "YAT SMART FARM V3.4 START"
  );


  // EEPROM

  EEPROM.begin(
    EEPROM_SIZE
  );


  // Relay

  relayInit();


  // RTC

  rtcInit();


  defaultSchedule();


  // WiFi




  // MQTT

  mqttInit();


  // OTA

  otaInit();


  startupCheck();


  publishDeviceInfo();


  sendDeviceState();

}


//==================== FINAL LOOP ====================

void loop(){

  ArduinoOTA.handle();





  runProduction();


  watchdogFeed();


  delay(10);

}
//==================== END SYSTEM FUNCTIONS ====================

// Manual Control Helper

void controlPump(bool state){

  autoMode = false;

  setRelay(
    RELAY_PUMP,
    state
  );

  saveConfig();

}


void controlZone1(bool state){

  autoMode = false;

  setRelay(
    RELAY_ZONE1,
    state
  );

  saveConfig();

}


void controlZone2(bool state){

  autoMode = false;

  setRelay(
    RELAY_ZONE2,
    state
  );

  saveConfig();

}


void controlLight(bool state){

  autoMode = false;

  setRelay(
    RELAY_LIGHT,
    state
  );

  saveConfig();

}


//==================== SYSTEM STATUS ====================

void printSystem(){

  Serial.println();

  Serial.println("YAT FARM STATUS");

  Serial.print("Mode : ");

  Serial.println(
    autoMode ? "AUTO":"MANUAL"
  );


  Serial.print("Pump : ");

  Serial.println(
    pumpState
  );


  Serial.print("Zone1 : ");

  Serial.println(
    zone1State
  );


  Serial.print("Zone2 : ");

  Serial.println(
    zone2State
  );


  Serial.print("Light : ");

  Serial.println(
    lightState
  );


  Serial.print("Time : ");

  Serial.println(
    getDateTime()
  );

}
//==================== FINAL MQTT CALLBACK ====================

void mqttCallback(char* topic,
                  byte* payload,
                  unsigned int length){

  String message = "";


  for(unsigned int i=0;i<length;i++){

    message += (char)payload[i];

  }


  Serial.print("RX: ");

  Serial.println(message);


  commandRouter(message);


  sendDeviceState();

}


//==================== MQTT CONNECT ====================

void reconnectMQTT(){

  while(!mqtt.connected()){


    Serial.println(
      "MQTT Connecting..."
    );


    if(mqtt.connect(
        "YAT-FARM-001"
       )){


      Serial.println(
        "MQTT Connected"
      );


      mqtt.subscribe(
        TOPIC_CONTROL
      );


      sendDeviceState();


    }

    else{


      Serial.print(
        "MQTT Failed "
      );


      Serial.println(
        mqtt.state()
      );


      delay(5000);


    }

  }

}


//==================== END ====================
//==================== WIFI RECONNECT ====================

void wifiReconnect(){

  if(WiFi.status() == WL_CONNECTED){

    return;

  }


  Serial.println(
    "WiFi Reconnecting..."
  );


  WiFi.disconnect();

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  unsigned long start =
    millis();


  while(WiFi.status()!=WL_CONNECTED){

    delay(500);


    if(millis()-start > 30000){

      Serial.println(
        "WiFi Timeout"
      );

      break;

    }

  }


  if(WiFi.status()==WL_CONNECTED){

    Serial.println(
      "WiFi OK"
    );

  }

}


//==================== NETWORK SERVICE ====================

void networkService(){

  wifiReconnect();


  if(!mqtt.connected()){

  

  }




}


//==================== FINAL SERVICE LOOP ====================

void mainService(){

  networkService();


  ArduinoOTA.handle();


  if(autoMode){

    autoPumpControl();

    autoZoneControl();

  }


  if(millis()-lastStatus > 10000){

    lastStatus = millis();

    sendDeviceState();

    printSystem();

  }


  watchdogFeed();

}
//==================== FIREBASE READY INTERFACE ====================

String firebaseDevicePath(){

  return "/devices/YAT-FARM-001";

}


String createFirebaseJSON(){

  StaticJsonDocument<512> doc;


  doc["deviceID"] =
    "YAT-FARM-001";


  doc["firmware"] =
    "V3.4";


  doc["online"] =
    true;


  doc["time"] =
    getDateTime();



  doc["relay"]["pump"] =
    pumpState;


  doc["relay"]["zone1"] =
    zone1State;


  doc["relay"]["zone2"] =
    zone2State;


  doc["relay"]["light"] =
    lightState;



  doc["mode"] =
    autoMode ? "AUTO":"MANUAL";


  if (dhtAvailable) {
    doc["sensors"]["temperature"] = temperature;
    doc["sensors"]["humidity"] = humidity;
  }


  String output;


  serializeJson(
    doc,
    output
  );


  return output;

}


//==================== FIREBASE SEND ====================

void firebaseUpdate(){

  String data =
    createFirebaseJSON();


  Serial.println(
    "Firebase Data:"
  );


  Serial.println(data);


  // Firebase RTDB Upload
  // ใส่ Firebase Client ในขั้นเชื่อมต่อจริง

}
//==================== FIREBASE COMMAND PARSER ====================

void firebaseCommand(String command){

  StaticJsonDocument<512> doc;


  DeserializationError error =
    deserializeJson(doc, command);


  if(error){

    Serial.println(
      "Firebase JSON ERROR"
    );

    return;

  }



  if(doc.containsKey("mode")){


    String mode =
      doc["mode"].as<String>();


    autoMode =
      (mode == "AUTO");


  }



  if(doc.containsKey("pump")){


    setRelay(
      RELAY_PUMP,
      doc["pump"]
    );


  }



  if(doc.containsKey("zone1")){


    setRelay(
      RELAY_ZONE1,
      doc["zone1"]
    );


  }



  if(doc.containsKey("zone2")){


    setRelay(
      RELAY_ZONE2,
      doc["zone2"]
    );


  }



  if(doc.containsKey("light")){


    setRelay(
      RELAY_LIGHT,
      doc["light"]
    );


  }



  updateConfig();


}


//==================== CLOUD SERVICE ====================

void cloudService(){

  firebaseUpdate();

  otaStatus();

}
//==================== FINAL PRODUCTION LOOP ====================

void productionService(){

  // Network

  networkService();


  // OTA

  otaService();


  // Auto Control

  if(autoMode){

    autoPumpControl();

    autoZoneControl();

  }


  // Cloud

  if(millis() - lastStatus > 10000){

    lastStatus = millis();


    cloudService();

    sendDeviceState();


    printSystem();

  }


  // Safety

  safetyCheck();


  // Watchdog

  watchdogFeed();

}


//==================== SYSTEM READY ====================

void systemReadyMessage(){

  Serial.println();
  Serial.println("==========================");
  Serial.println(" YAT SMART FARM V3.4 ");
  Serial.println(" REAL PRODUCTION READY ");
  Serial.println("==========================");

}
//==================== FINAL INITIALIZATION ====================

void initializeSystem(){

  Serial.println(
    "Initializing System..."
  );


  // Memory

  EEPROM.begin(
    EEPROM_SIZE
  );


  // Relay

  relayInit();


  // Load Config

  loadConfig();


  // RTC

  rtcInit();


  // Default Schedule

  defaultSchedule();


  // Network




  // MQTT

  mqttInit();


  // OTA

  otaInit();


  watchdogInit();


  systemReadyMessage();


  startupCheck();


}


//==================== FINAL LOOP HANDLER ====================

void handleSystem(){

  productionService();

  delay(10);

}
//==================== SYSTEM COMMAND EXTENSION ====================

void systemCommand(String cmd){


  if(cmd == "ALL_ON"){


    setRelay(RELAY_PUMP,true);

    setRelay(RELAY_ZONE1,true);

    setRelay(RELAY_ZONE2,true);

    setRelay(RELAY_LIGHT,true);


  }


  else if(cmd == "ALL_OFF"){


    relayAllOff();


  }


  else if(cmd == "AUTO"){


    autoMode = true;


  }


  else if(cmd == "MANUAL"){


    autoMode = false;


  }


  else if(cmd == "SAVE"){


    saveConfig();


  }


  else if(cmd == "STATUS"){


    sendDeviceState();


  }


  updateConfig();

}


//==================== COMMAND CHECK ====================

void commandCheck(){

  if(Serial.available()){


    String cmd =
      Serial.readStringUntil('\n');


    cmd.trim();


    systemCommand(cmd);


  }

}


//==================== DEBUG SERVICE ====================

void debugService(){

  commandCheck();

}
//==================== FINAL MAIN CONTROL ====================

void finalControl(){


  // ตรวจระบบเครือข่าย

  networkService();



  // OTA

  ArduinoOTA.handle();



  // รับคำสั่ง MQTT





  // ระบบ Auto

  if(autoMode){


    autoPumpControl();


    autoZoneControl();


  }



  // ส่งสถานะ

  if(millis() - lastStatus > 10000){


    lastStatus = millis();


    sendDeviceState();


    firebaseUpdate();


    printSystem();


  }



  // ตรวจความปลอดภัย

  safetyCheck();



  // ดูแลระบบ

  watchdogFeed();


}



//==================== SYSTEM LOOP READY ====================

void runSystem(){


  finalControl();


  debugService();


}
//==================== COMPLETE SYSTEM WRAPPER ====================

void systemLoop(){


  runSystem();


  // Memory Monitor

  static unsigned long memTimer = 0;


  if(millis() - memTimer > 60000){


    memTimer = millis();


    memoryStatus();


  }


}


//==================== FINAL BOOT ====================

void bootMessage(){


  Serial.println();

  Serial.println(
    "=============================="
  );

  Serial.println(
    "YAT SMART FARM V3.4"
  );

  Serial.println(
    "ESP8266 CONTROLLER"
  );

  Serial.println(
    "RTC + RELAY + MQTT + OTA"
  );

  Serial.println(
    "=============================="
  );


}
//==================== FINAL MAIN ENTRY ====================

// เรียกใช้ใน setup()

void startSystem(){

  bootMessage();

  initializeSystem();

  systemReadyMessage();

}


// เรียกใช้ใน loop()

void updateSystem(){

  systemLoop();

}


//==================== FINAL CHECK ====================

void finalCheck(){

  if(WiFi.status()!=WL_CONNECTED){

    Serial.println(
      "WiFi Warning"
    );

  }


  if(!mqtt.connected()){

    Serial.println(
      "MQTT Warning"
    );

  }


  if(!rtc.begin()){

    systemError(
      "RTC ERROR"
    );

  }


}


//==================== PRODUCTION MODE ====================

void productionMode(){

  finalCheck();

  updateSystem();

}

//==================== END MODULE ====================
//==================== FINAL LOOP SERVICE ====================

void serviceManager(){

  // Network Service

  networkService();


  // MQTT Service




  // OTA Service

  otaService();


  // Auto Water Service

  if(autoMode){

    autoPumpControl();

    autoZoneControl();

  }


  // Cloud Sync

  static unsigned long cloudTimer = 0;


  if(millis() - cloudTimer > 15000){


    cloudTimer = millis();


    firebaseUpdate();

    sendDeviceState();


  }


  // System Safety

  safetyCheck();


  // Watchdog

  watchdogFeed();


}


//==================== FINAL STATUS ====================

void statusReport(){

  Serial.println();

  Serial.println(
    "---- YAT STATUS ----"
  );


  Serial.println(
    deviceState()
  );


  Serial.println(
    "--------------------"
  );

}
//==================== REMOTE SETTINGS ====================

struct SystemSetting {

  bool autoEnable;

  uint8_t pumpStartHour;

  uint8_t pumpStartMinute;

  uint8_t pumpStopHour;

  uint8_t pumpStopMinute;

};


SystemSetting setting;


//==================== DEFAULT SETTING ====================

void loadDefaultSetting(){

  setting.autoEnable = true;

  setting.pumpStartHour = 6;

  setting.pumpStartMinute = 0;

  setting.pumpStopHour = 6;

  setting.pumpStopMinute = 30;

}


//==================== APPLY SETTING ====================

void applySetting(){

  autoMode =
    setting.autoEnable;


  pumpCloud.enable =
    setting.autoEnable;


  pumpCloud.startHour =
    setting.pumpStartHour;


  pumpCloud.startMinute =
    setting.pumpStartMinute;


  pumpCloud.stopHour =
    setting.pumpStopHour;


  pumpCloud.stopMinute =
    setting.pumpStopMinute;

}


//==================== SAVE SETTING ====================

void saveSetting(){

  EEPROM.put(
    32,
    setting
  );


  EEPROM.commit();


  Serial.println(
    "SETTING SAVED"
  );

}
//==================== LOAD SETTING ====================

void loadSetting(){

  EEPROM.get(
    32,
    setting
  );


  if(setting.pumpStartHour > 23){

    loadDefaultSetting();

  }


  applySetting();

}


//==================== REMOTE SETTING JSON ====================

void settingCommand(String data){

  StaticJsonDocument<256> doc;


  if(deserializeJson(doc,data)){

    return;

  }


  if(doc.containsKey("auto")){

    setting.autoEnable =
      doc["auto"];

  }


  if(doc.containsKey("startHour")){

    setting.pumpStartHour =
      doc["startHour"];

  }


  if(doc.containsKey("startMinute")){

    setting.pumpStartMinute =
      doc["startMinute"];

  }


  if(doc.containsKey("stopHour")){

    setting.pumpStopHour =
      doc["stopHour"];

  }


  if(doc.containsKey("stopMinute")){

    setting.pumpStopMinute =
      doc["stopMinute"];

  }


  applySetting();

  saveSetting();

}


//==================== END SETTING ====================
//==================== FINAL DEVICE REPORT ====================

String createStatusReport(){

  StaticJsonDocument<512> doc;


  doc["device"] =
    "YAT-FARM-001";


  doc["version"] =
    "3.4.0";


  doc["wifi"] =
    WiFi.status() == WL_CONNECTED;


  doc["ip"] =
    WiFi.localIP().toString();


  doc["time"] =
    getDateTime();


  doc["mode"] =
    autoMode ? "AUTO":"MANUAL";


  doc["relay"]["pump"] =
    pumpState;


  doc["relay"]["zone1"] =
    zone1State;


  doc["relay"]["zone2"] =
    zone2State;


  doc["relay"]["light"] =
    lightState;


  doc["memory"] =
    ESP.getFreeHeap();



  String output;


  serializeJson(
    doc,
    output
  );


  return output;

}


//==================== PUBLISH REPORT ====================

void publishReport(){

  String report =
    createStatusReport();


  Serial.println(report);


  mqtt.publish(
    TOPIC_STATUS,
    report.c_str(),
    true
  );

}
//==================== FINAL SECURITY ====================

const String DEVICE_KEY =
  "YAT-V34-SECURE";


//==================== CHECK DEVICE ====================

bool checkDeviceKey(String key){

  if(key == DEVICE_KEY){

    return true;

  }


  return false;

}


//==================== SECURE COMMAND ====================

void secureCommand(String data){

  StaticJsonDocument<256> doc;


  if(deserializeJson(doc,data)){

    return;

  }


  String key =
    doc["key"].as<String>();


  if(!checkDeviceKey(key)){

    Serial.println(
      "ACCESS DENIED"
    );

    return;

  }


  if(doc.containsKey("command")){


    String cmd =
      doc["command"].as<String>();


    systemCommand(cmd);


  }


}


//==================== SECURITY STATUS ====================

void securityStatus(){

  Serial.println(
    "SECURITY ENABLED"
  );

}
//==================== FINAL DEVICE ID ====================

String deviceID(){

  return "YAT-FARM-001";

}


//==================== DEVICE CONFIG JSON ====================

String deviceConfigJSON(){

  StaticJsonDocument<256> doc;


  doc["id"] =
    deviceID();


  doc["firmware"] =
    firmwareVersion();


  doc["controller"] =
    "ESP8266";


  doc["relay"] =
    "4CH";


  doc["sensor"] =
    "NONE";


  doc["cloud"] =
    "MQTT+Firebase";


  String data;


  serializeJson(
    doc,
    data
  );


  return data;

}


//==================== SEND CONFIG ====================

void sendConfig(){

  String data =
    deviceConfigJSON();




}
//==================== FINAL HEALTH MONITOR ====================

unsigned long bootTime = 0;


//==================== INIT HEALTH ====================

void healthInit(){

  bootTime = millis();

}


//==================== HEALTH DATA ====================

String healthData(){

  StaticJsonDocument<256> doc;


  doc["device"] =
    deviceID();


  doc["uptime"] =
    (millis()-bootTime)/1000;


  doc["heap"] =
    ESP.getFreeHeap();


  doc["wifi"] =
    WiFi.RSSI();


  doc["mqtt"] =
    mqtt.connected();


  String data;


  serializeJson(
    doc,
    data
  );


  return data;

}


//==================== SEND HEALTH ====================

void sendHealth(){

  String data =
    healthData();




}
//==================== FINAL SCHEDULE STATUS ====================

String scheduleStatus(){

  StaticJsonDocument<256> doc;


  doc["pump"]["enable"] =
    pumpCloud.enable;


  doc["pump"]["start"] =
    String(pumpCloud.startHour)
    + ":" +
    String(pumpCloud.startMinute);


  doc["pump"]["stop"] =
    String(pumpCloud.stopHour)
    + ":" +
    String(pumpCloud.stopMinute);


  String data;


  serializeJson(
    doc,
    data
  );


  return data;

}


//==================== SEND SCHEDULE ====================

void sendSchedule(){

  String data =
    scheduleStatus();




}


//==================== SCHEDULE SERVICE ====================

void scheduleService(){

  if(autoMode){

    autoPumpControl();

  }

}
//==================== FINAL SYSTEM SERVICE ====================

void systemService(){


  // Network

  networkService();



  // Schedule

  scheduleService();



  // OTA

  otaService();



  // Cloud Update

  static unsigned long cloudUpdate = 0;


  if(millis() - cloudUpdate > 15000){


    cloudUpdate = millis();


    publishReport();


    sendHealth();


    sendSchedule();


  }



  // Safety

  safetyCheck();



  // Memory

  watchdogFeed();

}


//==================== FINAL DEBUG ====================

void debugOutput(){

  Serial.println(
    "YAT FARM V3.4 RUNNING"
  );


  Serial.println(
    createStatusReport()
  );

}
//==================== FINAL SETUP LINK ====================

void setupFinal(){

  bootMessage();


  dht.begin();


  initializeSystem();


  healthInit();


  securityStatus();


  sendConfig();


  sendDeviceState();


  Serial.println(
    "SYSTEM INITIALIZED"
  );

}


//==================== FINAL LOOP LINK ====================

void loopFinal(){

  systemService();


  debugService();


  delay(10);

}
//==================== FINAL COMMAND DISPATCH ====================

void dispatchCommand(String command){


  command.trim();



  if(command.startsWith("SET")){


    settingCommand(
      command.substring(3)
    );


  }


  else if(command.startsWith("{")){


    handleMQTTCommand(
      command
    );


  }


  else{


    systemCommand(
      command
    );


  }



  sendDeviceState();

}


//==================== SERIAL CONTROL ====================

void serialControl(){


  if(Serial.available()){


    String cmd =
      Serial.readStringUntil('\n');


    dispatchCommand(cmd);


  }


}


//==================== FINAL INPUT SERVICE ====================

void inputService(){


  serialControl();


}
//==================== FINAL RUNNER ====================

void runFinalSystem(){


  inputService();


  systemService();





  ArduinoOTA.handle();


  watchdogFeed();



}


//==================== DEVICE READY ====================

void deviceReady(){

  Serial.println();

  Serial.println(
    "=========================="
  );

  Serial.println(
    "YAT SMART FARM V3.4 ONLINE"
  );

  Serial.println(
    "PUMP / ZONE1 / ZONE2 / LIGHT"
  );

  Serial.println(
    "MQTT + FIREBASE + OTA"
  );

  Serial.println(
    "=========================="
  );

}
//==================== FINAL BOOT SEQUENCE ====================

void bootSequence(){


  Serial.begin(115200);


  delay(500);



  deviceReady();



  setupFinal();



}


//==================== FINAL MAIN LOOP ====================

void mainLoop(){


  runFinalSystem();


  static unsigned long reportTimer = 0;


  if(millis() - reportTimer > 30000){


    reportTimer = millis();


    debugOutput();


  }


}


//==================== END CONTROLLER ===================
//==================== FINAL WRAPPER ====================

// ใช้แทน setup()

void setup(){

  bootSequence();

}


// ใช้แทน loop()

void loop(){

  mainLoop();

}


//==================== BUILD INFORMATION ====================

const char* BUILD_NAME =
  "YAT-SmartFarm-V3.4-REAL-PRODUCTION";


const char* BUILD_VERSION =
  "3.4.0";


void buildInfo(){

  Serial.println(
    BUILD_NAME
  );


  Serial.println(
    BUILD_VERSION
  );

}


//==================== END FILE ====================
//==================== FINAL COMPLETE CHECK ====================

void completeCheck(){

  Serial.println(
    "SYSTEM CHECK"
  );


  Serial.print(
    "WiFi : "
  );

  Serial.println(
    WiFi.status()==WL_CONNECTED ?
    "OK":"FAIL"
  );


  Serial.print(
    "MQTT : "
  );

  Serial.println(
    mqtt.connected() ?
    "OK":"FAIL"
  );


  Serial.print(
    "RTC : "
  );

  Serial.println(
    rtc.begin() ?
    "OK":"FAIL"
  );


  Serial.print(
    "Relay : "
  );

  Serial.println(
    "READY"
  );


}


//==================== FINAL START ====================

void productionStart(){

  buildInfo();

  completeCheck();

  publishReport();

  sendHealth();

}
//==================== FINAL SYSTEM CONTROL ====================

void masterControl(){

  // ตรวจ Auto Mode

  if(autoMode){


    autoPumpControl();


    autoZoneControl();


  }



  // ส่งสถานะทุกระบบ


  static unsigned long statusTime = 0;


  if(millis() - statusTime > 10000){


    statusTime = millis();


    publishReport();


    firebaseUpdate();


  }



  // ป้องกันระบบค้าง

  watchdogFeed();


}



//==================== FINAL DEVICE LOOP ====================

void deviceLoop(){


  masterControl();


  networkService();


  otaService();





}
//==================== FINAL RUN ====================

void finalRun(){


  deviceLoop();


  inputService();


  static unsigned long healthTimer = 0;


  if(millis() - healthTimer > 60000){


    healthTimer = millis();


    sendHealth();


    memoryStatus();


  }


}


//==================== FINAL SHUTDOWN ====================

void safeShutdown(){


  relayAllOff();


  saveConfig();


  Serial.println(
    "SYSTEM SHUTDOWN SAFE"
  );


}
//==================== FINAL COMMAND MANAGER ====================

void commandManager(String cmd){


  cmd.trim();


  if(cmd.startsWith("RESET") ||
     cmd.startsWith("FACTORY") ||
     cmd.startsWith("REPORT")){


    extendedCommand(cmd);


  }


  else{


    dispatchCommand(cmd);


  }


}


//==================== SERIAL MANAGER ====================

void serialManager(){


  if(Serial.available()){


    String cmd =
      Serial.readStringUntil('\n');


    commandManager(cmd);


  }


}


//==================== FINAL INPUT ====================

void finalInput(){


  serialManager();


}
//==================== FINAL OPERATION MODE ====================

enum SystemMode {

  MODE_AUTO,

  MODE_MANUAL,

  MODE_SAFE

};


SystemMode currentMode = MODE_AUTO;


//==================== MODE CONTROL ====================

void setSystemMode(SystemMode mode){

  currentMode = mode;


  if(mode == MODE_SAFE){

    relayAllOff();

  }


}


//==================== MODE STATUS ====================

String modeName(){


  if(currentMode == MODE_AUTO){

    return "AUTO";

  }


  if(currentMode == MODE_MANUAL){

    return "MANUAL";

  }


  return "SAFE";

}


//==================== MODE SERVICE ====================

void modeService(){


  if(currentMode == MODE_AUTO){


    autoPumpControl();


    autoZoneControl();


  }


  else if(currentMode == MODE_SAFE){


    relayAllOff();


  }


}
//==================== FINAL STATUS WITH MODE ====================

String fullStatus(){

  StaticJsonDocument<512> doc;


  doc["device"] =
    deviceID();


  doc["version"] =
    firmwareVersion();


  doc["mode"] =
    modeName();


  doc["time"] =
    getDateTime();


  doc["relay"]["pump"] =
    pumpState;


  doc["relay"]["zone1"] =
    zone1State;


  doc["relay"]["zone2"] =
    zone2State;


  doc["relay"]["light"] =
    lightState;


  doc["heap"] =
    ESP.getFreeHeap();


  String data;


  serializeJson(
    doc,
    data
  );


  return data;

}


//==================== SEND FULL STATUS ====================

void sendFullStatus(){

  String data =
    fullStatus();





  Serial.println(data);

}
//==================== FINAL MQTT STATUS SERVICE ====================

void mqttStatusService(){


  static unsigned long mqttTimer = 0;


  if(millis() - mqttTimer > 10000){


    mqttTimer = millis();


    sendFullStatus();


  }


}


//==================== FINAL CLOUD SERVICE ====================

void finalCloudService(){


  if(!mqtt.connected()){

  

  }





  firebaseUpdate();


}


//==================== FINAL SAFETY SERVICE ====================

void finalSafety(){


  if(currentMode == MODE_SAFE){

    relayAllOff();

    return;

  }


  if(WiFi.status()!=WL_CONNECTED){

    Serial.println(
      "NETWORK LOST"
    );

  }


  watchdogFeed();

}
//==================== FINAL PRODUCTION TASK ====================

void productionTask(){


  // รับคำสั่ง

  finalInput();



  // Network

  networkService();



  // Cloud

  finalCloudService();



  // Mode Control

  modeService();



  // OTA

  otaService();



  // Status

  mqttStatusService();



  // Safety

  finalSafety();


}


//==================== FINAL APPLICATION LOOP ====================

void applicationLoop(){


  readDHT11();


  productionTask();


  delay(10);


}
//==================== FINAL APPLICATION START ====================

void applicationStart(){


  bootMessage();


  dht.begin();


  initializeSystem();


  loadSetting();


  healthInit();


  productionStart();


  deviceReady();


}


//==================== FINAL APPLICATION RUN ====================

void applicationRun(){


  applicationLoop();


}


//==================== END APPLICATION CORE ====================
//==================== FINAL ENTRY POINT ====================

// ใช้เป็น setup หลัก

void setup(){

  applicationStart();

}


// ใช้เป็น loop หลัก

void loop(){

  applicationRun();

}


//==================== PROJECT END ====================

/*

YAT-SmartFarm-V3.4-REAL-PRODUCTION

Features:

- ESP8266 NodeMCU
- RTC DS3231
- Relay 4CH
    Pump
    Zone1
    Zone2
    Light

- MQTT Command
- Firebase Ready
- OTA Update
- Admin Dashboard Ready
- Auto Schedule
- Manual Control
- Safety System

*/


/*
DHT11 integration notes:
- Sensor type: DHT11
- Data pin: D4
- The DHT11 is for air temperature and relative humidity.
- Soil moisture should remain the primary input for irrigation decisions.
- The helper dht11Json() is available to expose readings from the existing
  /api/sensors handler without changing the existing API structure.
*/
