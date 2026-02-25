#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <ClosedCube_HDC1080.h>
#include <Adafruit_BMP085.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include "mbedtls/sha256.h"

// --- Function Prototypes ---

void drawNormalScreen(float temp, float hum, float pres, int rssi);
void drawTransitionScreen(String oldState, String newState);

void drawSignalIcon(int x, int y, int rssi);
void drawStatusIcons(int x, int y, bool wifiOk, bool mqttOk, bool uploading);

// --- Configuration ---
const char* WIFI_SSID = "TRGOZ.DAG";
const char* WIFI_PASS = "trgoz.dag";
const char* FIRMWARE_VERSION = "0.0.8"; 

const char* API_URLS[] = {
    "http://10.141.5.226:7120"
};
const int API_URL_COUNT = 1;

const char* MQTT_BROKER_URLS[] = {
    "http://10.141.5.226:1883"
};
const int MQTT_BROKER_COUNT = 1;
const int MQTT_PORT = 1883;

// --- Hardware ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

ClosedCube_HDC1080 hdc1080;
Adafruit_BMP085 bmp;

// --- State Definitions ---
enum AppState {
    STATE_INIT,
    STATE_SHOW_INFO, // New state for Boot/Blink
    STATE_CONNECT_WIFI,
    STATE_CHECK_IS_REGISTERED,
    STATE_REGISTER_DEVICE,
    STATE_WAIT_APPROVAL,
    STATE_CHECK_UPDATE,
    STATE_UPDATE_FIRMWARE,
    STATE_GET_CONFIG,
    STATE_RUNNING
};

AppState currentState = STATE_INIT;
AppState returnState = STATE_INIT; // State to return to after info screen

// --- Variables ---
String macAddress;
String macHash;
String deviceId = "0"; 
String apiKey = "";
String manageApiKey = ""; 
int dataInterval = 15000; 
String location = "";
String deviceName = "";
String commandPrefix = ""; // New: for MQTT commands

WiFiClient mqttClientNet; 
WiFiClientSecure mqttClientNetSecure;
PubSubClient mqttClient(mqttClientNet);

unsigned long lastRegistrationCheck = 0;
const unsigned long REGISTRATION_CHECK_INTERVAL = 10000; 

unsigned long lastMsg = 0; // Will be reset for immediate first read
unsigned long infoScreenStart = 0;
const unsigned long INFO_SCREEN_DURATION = 10000;
float lastTemp = 0;
float lastHum = 0;
float lastPres = 0;
int lastRssi = 0;
int headerScrollPos = 0;
unsigned long lastHeaderUpdate = 0;
const unsigned long HEADER_REFRESH_INTERVAL = 100; // Increased speed (was 300)
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_READ_INTERVAL = 1000; 
unsigned long lastUploadTime = 0;
const unsigned long UPLOAD_ICON_DURATION = 700;

// Running statistics for transmitted sensor values
unsigned long statsCount = 0;
double tempSum = 0;
double humSum = 0;
double presSum = 0;
float tempMin = 0;
float tempMax = 0;
float humMin = 0;
float humMax = 0;
float presMin = 0;
float presMax = 0;
bool statsInitialized = false;

// Helper to get state string
String getStateName(AppState state) {
// ... (unchanged)
    switch(state) {
        case STATE_INIT: return "Init";
        case STATE_SHOW_INFO: return "Info";
        case STATE_CONNECT_WIFI: return "WiFi";
        case STATE_CHECK_IS_REGISTERED: return "Check Reg";
        case STATE_REGISTER_DEVICE: return "Register";
        case STATE_WAIT_APPROVAL: return "Approval";
        case STATE_CHECK_UPDATE: return "Update";
        case STATE_UPDATE_FIRMWARE: return "Flashing";
        case STATE_GET_CONFIG: return "Config";
        case STATE_RUNNING: return "Running";
        default: return "Unknown";
    }
}

void changeState(AppState newState) {
    if (currentState != newState && currentState != STATE_SHOW_INFO && newState != STATE_SHOW_INFO) {
        String oldName = getStateName(currentState);
        String newName = getStateName(newState);
        drawTransitionScreen(oldName, newName);
    }
    currentState = newState;
    
    // If entering RUNNING state, restore the dashboard immediately
    if (currentState == STATE_RUNNING) {
        drawNormalScreen(lastTemp, lastHum, lastPres, WiFi.RSSI());
    }
}

// --- Hashing Utility ---
String generateStrongId(const uint8_t mac[6]) {
  // 1️⃣ SHA256 hesapla
  uint8_t hash[32];
  mbedtls_sha256(mac, 6, hash, 0);

  // 2️⃣ İlk 48 bit al
  uint64_t value = 0;
  for (int i = 0; i < 6; i++) {
    value = (value << 8) | hash[i];
  }

  // 3️⃣ Base36 (6 karakter)
  const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  char result[7];
  result[6] = '\0';

  const uint64_t mod = 2176782336ULL; // 36^6
  value = value % mod;

  for (int i = 5; i >= 0; i--) {
    result[i] = charset[value % 36];
    value /= 36;
  }

  return String(result);
}
// --- Display Helpers ---
void updateDisplay(String line1, String line2, String line3 = "") {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.println(line1);
    display.println(line2);
    display.println(line3);
    display.display();
}

void drawInfoScreen() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    // Large Device ID (selim1/selim2)
    display.setTextSize(2);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(deviceId, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 10);
    display.println(deviceId);

    // Small MAC Address
    display.setTextSize(1);
    display.getTextBounds(macAddress, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 40);
    display.println(macAddress);

    display.display();
}

void drawNormalScreen(float temp, float hum, float pres, int rssi) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setTextWrap(false); 

    // --- Sidebar Config ---
    int sidebarX = 105;
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    bool mqttOk = mqttClient.connected();
    bool uploading = (millis() - lastUploadTime < UPLOAD_ICON_DURATION);

    // --- Header ---
    String header = (location.length() > 0 && deviceName.length() > 0) ? (location + " / " + deviceName) : 
                    (deviceName.length() > 0 ? deviceName : (location.length() > 0 ? location : deviceId));
    header += "    ";

    int maxHeaderWidth = sidebarX - 5;
    int16_t hx1, hy1; uint16_t hw, hh;
    display.getTextBounds(header, 0, 0, &hx1, &hy1, &hw, &hh);

    if (hw > maxHeaderWidth) {
        int scrollRange = hw - maxHeaderWidth + 5; 
        int offset = (headerScrollPos / 1) % (scrollRange + 30);
        if (offset > scrollRange) offset = scrollRange;
        display.setCursor(-offset, 0);
    } else {
        display.setCursor(0, 0);
    }
    display.print(header);
    // Mask the sidebar area to prevent scrolling text from overlapping
    display.fillRect(sidebarX - 3, 0, SCREEN_WIDTH - (sidebarX - 3), SCREEN_HEIGHT, SSD1306_BLACK);
    
    // --- Sidebar Column ---
    display.drawFastVLine(sidebarX - 4, 0, SCREEN_HEIGHT, SSD1306_WHITE);
    
    // 1) Signal Icon (Header Right)
    drawSignalIcon(sidebarX, 0, rssi);

    // 2) Status Indicators
    drawStatusIcons(sidebarX, 15, wifiOk, mqttOk, uploading);

    // 3) Numeric RSSI
    display.setCursor(sidebarX, 55);
    display.print(rssi);

    display.drawLine(0, 10, sidebarX - 4, 10, SSD1306_WHITE);

    // --- Sensors ---
    display.setCursor(0, 20);
    display.print("Temp: "); display.print(temp, 2); display.println(" C");
    display.setCursor(0, 35);
    display.print("Hum : "); display.print(hum, 2); display.println(" %");
    display.setCursor(0, 50);
    display.print("Pres: "); display.print(pres, 2); display.println(" hPa");

    display.display();
}

void drawSignalIcon(int x, int y, int rssi) {
    // 4 Bars like signal icon
    int bars = 0;
    if (rssi > -50) bars = 4;
    else if (rssi > -70) bars = 3;
    else if (rssi > -85) bars = 2;
    else if (rssi > -100) bars = 1;

    for (int i = 0; i < 4; i++) {
        int h = (i + 1) * 2;
        if (i < bars) {
            display.fillRect(x + (i * 4), y + 8 - h, 3, h, SSD1306_WHITE);
        } else {
            display.drawRect(x + (i * 4), y + 8 - h, 3, h, SSD1306_WHITE);
        }
    }
}

void drawStatusIcons(int x, int y, bool wifiOk, bool mqttOk, bool uploading) {
    // WiFi Status (W)
    display.setCursor(x, y);
    if (!wifiOk) display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.print("WF");
    display.setTextColor(SSD1306_WHITE);

    // MQTT Status (M)
    display.setCursor(x, y + 12);
    if (!mqttOk) display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.print("MQ");
    display.setTextColor(SSD1306_WHITE);

    // Upload Arrow (U)
    if (uploading) {
        // Draw small up arrow
        display.drawTriangle(x + 5, y + 25, x + 1, y + 29, x + 9, y + 29, SSD1306_WHITE);
        display.fillRect(x + 4, y + 29, 3, 5, SSD1306_WHITE);
    }
}

void drawTransitionScreen(String oldState, String newState) {
    if (oldState == newState) return;
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // Old State (Top)
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(oldState, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 5);
    display.println(oldState);

    // Arrow (Center)
    display.setCursor((SCREEN_WIDTH - 12) / 2, 25);
    display.println(">>");

    // New State (Bottom)
    display.getTextBounds(newState, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 45);
    display.println(newState);

    display.display();
    delay(1500); // Show transition for 1.5s
}

// --- API Client ---
String makeRequest(String endpoint, String params, String method = "GET", String payload = "") {
    if (WiFi.status() != WL_CONNECTED) return "";

    for (int i = 0; i < API_URL_COUNT; i++) {
        HTTPClient http;
        String fullUrl = String(API_URLS[i]) + endpoint;
        if (method == "GET") {
            fullUrl += params;
        }
        
        // Handle secure/insecure connection
        if (fullUrl.startsWith("https")) {
             WiFiClientSecure client;
             client.setInsecure(); 
             http.begin(client, fullUrl); 
        } else {
             http.begin(fullUrl);
        }

        http.setConnectTimeout(2000); 
        
        int httpCode;
        if (method == "POST") {
            http.addHeader("Content-Type", "application/json");
            httpCode = http.POST(payload);
        } else {
            httpCode = http.GET();
        }

        if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            http.end();
            return response;
        } 
        http.end();
    }
    return "";
}

// --- OTA Update ---
void checkOTA(String file_path) {
    if (file_path == "" || file_path == "null") {
        Serial.println("OTA: Invalid file path.");
        return;
    }
    
    Serial.println("--- OTA Update Debug ---");
    Serial.println("Source URL: " + file_path);
    updateDisplay("Update Found!", "Downloading...", "Wait...");
    
    // Stop other tasks
    esp_task_wdt_delete(NULL); 
    
    t_httpUpdate_return ret;
    
    if (file_path.startsWith("https")) {
        Serial.println("Mode: HTTPS (Insecure)");
        WiFiClientSecure client;
        client.setInsecure();
        ret = httpUpdate.update(client, file_path);
    } else {
        Serial.println("Mode: HTTP");
        WiFiClient client;
        ret = httpUpdate.update(client, file_path);
    }

    switch (ret) {
      case HTTP_UPDATE_FAILED:
        Serial.printf("OTA: FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
        updateDisplay("Update Failed", httpUpdate.getLastErrorString());
        delay(5000);
        ESP.restart(); // Restart to recover WDT and state
        break;

      case HTTP_UPDATE_NO_UPDATES:
        Serial.println("OTA: NO_UPDATES");
        break;

      case HTTP_UPDATE_OK:
        Serial.println("OTA: OK (Rebooting)");
        break;
    }
}


// --- MQTT Callback ---
void callback(char* topic, byte* payload, unsigned int length) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) {
        Serial.print(F("MQTT JSON parse failed: "));
        Serial.println(error.f_str());
        return;
    }

    const char* command = doc["command"];
    if (!command) return;

    Serial.println("Message arrived [" + String(topic) + "] " + String(command));

    if (strcmp(command, "restart") == 0) {
        ESP.restart();
    } else if (strcmp(command, "blink") == 0) {
        if (currentState != STATE_SHOW_INFO) {
            returnState = currentState;
            currentState = STATE_SHOW_INFO;
            infoScreenStart = millis();
            drawInfoScreen();
        }
    } else if (strcmp(command, "newfirmware") == 0) {
        changeState(STATE_CHECK_UPDATE);
    } else if (strcmp(command, "newconfig") == 0) {
        changeState(STATE_GET_CONFIG);
    }
}

void reconnectMQTT() {
    if(WiFi.status() != WL_CONNECTED) return;
    
    if (!mqttClient.connected()) {
        esp_task_wdt_reset(); 
        String clientId = "ESP32Sensor-" + macHash;

        for (int i = 0; i < MQTT_BROKER_COUNT; i++) {
            String url = String(MQTT_BROKER_URLS[i]);
            Serial.print("Attempting MQTT connection: " + url + "...");
            
            bool isSecure = url.startsWith("https");
            String host = url;
            int port = MQTT_PORT;

            // Simple parser
            int protoEnd = url.indexOf("://");
            if (protoEnd != -1) host = url.substring(protoEnd + 3);
            
            int portStart = host.indexOf(":");
            if (portStart != -1) {
                port = host.substring(portStart + 1).toInt();
                host = host.substring(0, portStart);
            }

            if (isSecure) {
                mqttClientNetSecure.setInsecure();
                mqttClient.setClient(mqttClientNetSecure);
            } else {
                mqttClient.setClient(mqttClientNet);
            }

            mqttClient.setServer(host.c_str(), port);

            if (mqttClient.connect(clientId.c_str())) {
                Serial.println("connected");
                String topic = "sensornet/commands/" + commandPrefix + deviceId;
                mqttClient.subscribe(topic.c_str());
                Serial.println("Subscribed to: " + topic);
                return; // Connection successful
            } else {
                Serial.println("failed, rc=" + String(mqttClient.state()));
                esp_task_wdt_reset();
            }
        }
    }
}



// --- Setup ---
void setup() {
    Serial.begin(115200);
    Wire.begin();

    // Init Display
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
    }
    display.clearDisplay();
    // updateDisplay("Sensornet", "Booting...", FIRMWARE_VERSION); // Removed for new boot logic
    
    // Init Sensors
    hdc1080.begin(0x40);
    if (!bmp.begin()) {
        Serial.println("Could not find BMP180 sensor!");
    }

    // Get MAC and Hash
    macAddress = WiFi.macAddress();
    uint8_t mac[6];
    WiFi.macAddress(mac);
    macHash = generateStrongId(mac);
    manageApiKey = macHash;
    
    // Determine device ID based on MAC address last byte
    uint8_t lastMacByte = macAddress.charAt(macAddress.length() - 1) - 48;
    if (lastMacByte % 2 == 0) {
        deviceId = "selim2";
    } else {
        deviceId = "selim2";
    }

    Serial.println("MAC: " + macAddress);
    Serial.println("Hash: " + macHash);
    Serial.println("Device ID: " + deviceId);

    // WDT Setup (120 saniye)
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 120000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
#else
    esp_task_wdt_init(120, true);
#endif
    esp_task_wdt_add(NULL);

    mqttClient.setCallback(callback); 
    
    // Initial State: Show Info
    currentState = STATE_SHOW_INFO;
    returnState = STATE_CONNECT_WIFI;
    infoScreenStart = millis();
    drawInfoScreen();
}

void loop() {
    esp_task_wdt_reset();

    switch (currentState) {
        case STATE_SHOW_INFO:
            drawInfoScreen();
            if (millis() - infoScreenStart > INFO_SCREEN_DURATION) {
                changeState(returnState);
            }
            break;

        case STATE_CONNECT_WIFI:
            if (WiFi.status() != WL_CONNECTED) {
                 Serial.println("Connecting to WiFi...");
                 updateDisplay("WiFi Connecting...", WIFI_SSID);
                 WiFi.begin(WIFI_SSID, WIFI_PASS);
                 
                 unsigned long startAttempt = millis();
                 while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
                     delay(500);
                     Serial.print(".");
                 }
                 Serial.println();
            }
            
            if (WiFi.status() == WL_CONNECTED) {
                changeState(STATE_CHECK_IS_REGISTERED);
            }
            break;

        case STATE_CHECK_IS_REGISTERED: {
            updateDisplay("Checking", "Registration...");
            String jsonRes = makeRequest("/get_status", "?mac_address=" + macAddress + "&manage_api_key=" + manageApiKey);
            
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, jsonRes);

            if (!error) {
                const char* status = doc["status"];
                if (status) { // Check for null
                    if (strcmp(status, "not_registered") == 0) {
                        changeState(STATE_REGISTER_DEVICE);
                    } else if (strcmp(status, "pending") == 0) {
                        changeState(STATE_WAIT_APPROVAL);
                        lastRegistrationCheck = millis();
                    } else if (strcmp(status, "registered") == 0) {
                        changeState(STATE_CHECK_UPDATE);
                    }
                }
            } else {
                 // API unreachable - bypass to direct MQTT test (TEMPORARY)
                 Serial.println("API unreachable, skipping to config...");
                 changeState(STATE_GET_CONFIG);
                 delay(2000);
            }
            break;
        }

        case STATE_REGISTER_DEVICE: {
            updateDisplay("Registering", "Device...");
            
            // Create JSON payload
            JsonDocument reqDoc;
            reqDoc["mac_address"] = macAddress;
            reqDoc["mac_hash"] = macHash;
            reqDoc["manage_api_key"] = manageApiKey;
            String reqPayload;
            serializeJson(reqDoc, reqPayload);

            // POST request
            String jsonRes = makeRequest("/register_device", "", "POST", reqPayload);
            
             JsonDocument doc;
             DeserializationError error = deserializeJson(doc, jsonRes);
             if (!error) {
                 // Assuming successful request leads to pending state check
                 changeState(STATE_WAIT_APPROVAL);
                 lastRegistrationCheck = millis();
             } else {
                 delay(5000);
             }
            break;
        }

        case STATE_WAIT_APPROVAL:
            updateDisplay("Waiting", "Approval...", "Pending");
            delay(1000); // Small delay
            if (millis() - lastRegistrationCheck > REGISTRATION_CHECK_INTERVAL) {
                 changeState(STATE_CHECK_IS_REGISTERED);
            }
            break;

        case STATE_CHECK_UPDATE: {
             updateDisplay("Checking", "Updates...");
             String jsonRes = makeRequest("/check_update", "?mac_address=" + macAddress + "&mac_hash=" + macHash + "&firmware_version=" + String(FIRMWARE_VERSION));
             
             JsonDocument doc;
             DeserializationError error = deserializeJson(doc, jsonRes);

             if (!error) {
                 if (doc["file_path"]) {
                     const char* filePath = doc["file_path"];
                     if (filePath != nullptr && strlen(filePath) > 0) {
                         checkOTA(String(filePath));
                     }
                 }
                 changeState(STATE_GET_CONFIG);
             } else {
                 changeState(STATE_GET_CONFIG);
             }
             
             // Show current version for 3 seconds before next state
             updateDisplay("Firmware Version", String(FIRMWARE_VERSION), "Current");
             delay(3000);

             break;
        }

        case STATE_GET_CONFIG: {
            updateDisplay("Fetching", "Config...");
            String jsonRes = makeRequest("/get_config", "?mac_address=" + macAddress + "&manage_api_key=" + manageApiKey);
            
            JsonDocument doc; 
            DeserializationError error = deserializeJson(doc, jsonRes);

            if (!error) {
                if (doc["id"]) deviceId = doc["id"].as<String>();
                if (doc["data_interval"]) dataInterval = doc["data_interval"].as<int>();
                if (doc["api_key"]) apiKey = doc["api_key"].as<String>();
                if (doc["location"]) location = doc["location"].as<String>();
                if (doc["name"]) deviceName = doc["name"].as<String>();
                if (doc["command_prefix"]) commandPrefix = doc["command_prefix"].as<String>();
                
                Serial.println("Config Loaded. ID: " + deviceId + " Prefix: " + commandPrefix);
            } else {
                // API unreachable - use default config (TEMPORARY TEST MODE)
                // Determine device ID based on MAC address last byte
                uint8_t lastMacByte = macAddress.charAt(macAddress.length() - 1) - 48; // Convert to number
                
                if (lastMacByte % 2 == 0) {
                    deviceId = "selim2";
                    deviceName = "Selim2";
                    location = "Lab";
                } else {
                    deviceId = "selim2";
                    deviceName = "Selim2";
                    location = "Field";
                }
                
                dataInterval = 15000;
                commandPrefix = "sensor_";
                
                Serial.println("Using default config. Device ID: " + deviceId + " | Name: " + deviceName);
            }
            
            // Show summary for 3 seconds
            updateDisplay(location, deviceName, "Int: " + String(dataInterval / 1000) + "s");
            delay(3000);

            // Force immediate sensor read on first run
            lastMsg = millis() - dataInterval - 1000;
            
            changeState(STATE_RUNNING);
            break;
        }

        case STATE_RUNNING: {
            if (WiFi.status() != WL_CONNECTED) {
                changeState(STATE_CONNECT_WIFI);
                break;
            }
            
            if (!mqttClient.connected()) {
                reconnectMQTT();
            }
            mqttClient.loop();

            unsigned long now = millis();

            // Scrolling Animation Refresh
            if (now - lastHeaderUpdate > HEADER_REFRESH_INTERVAL) {
                lastHeaderUpdate = now;
                headerScrollPos += 1; // Move 1 pixel for smoothness (was 2)
                drawNormalScreen(lastTemp, lastHum, lastPres, lastRssi);
            }

            // Sensor Reading & Display Refresh (Every 5 seconds)
            if (now - lastSensorRead > SENSOR_READ_INTERVAL || lastSensorRead == 0) {
                lastSensorRead = now;
                lastTemp = hdc1080.readTemperature();
                lastHum = hdc1080.readHumidity();
                lastPres = bmp.readPressure() / 100.0F;

                // Format to 2 decimal places
                lastTemp = round(lastTemp * 100) / 100.0;
                lastHum = round(lastHum * 100) / 100.0;
                lastPres = round(lastPres * 100) / 100.0;
                lastRssi = WiFi.RSSI();

                if (!statsInitialized) {
                    tempMin = tempMax = lastTemp;
                    humMin = humMax = lastHum;
                    presMin = presMax = lastPres;
                    statsInitialized = true;
                } else {
                    if (lastTemp < tempMin) tempMin = lastTemp;
                    if (lastTemp > tempMax) tempMax = lastTemp;
                    if (lastHum < humMin) humMin = lastHum;
                    if (lastHum > humMax) humMax = lastHum;
                    if (lastPres < presMin) presMin = lastPres;
                    if (lastPres > presMax) presMax = lastPres;
                }

                tempSum += lastTemp;
                humSum += lastHum;
                presSum += lastPres;
                statsCount++;

                drawNormalScreen(lastTemp, lastHum, lastPres, lastRssi);
                Serial.println("Sensors updated on screen.");
            }

            // Data Transmission (Every dataInterval)
            if (now - lastMsg > (unsigned long)dataInterval) {
                lastMsg = now;
                
                // Use already read values (they are updated every 5s)
                JsonDocument jsonDoc;
                jsonDoc["temp"] = lastTemp;
                jsonDoc["hum"] = lastHum;
                jsonDoc["pres"] = lastPres;
                if (statsInitialized && statsCount > 0) {
                    jsonDoc["temp_min"] = tempMin;
                    jsonDoc["temp_max"] = tempMax;
                    jsonDoc["temp_avg"] = round((tempSum / statsCount) * 100.0) / 100.0;

                    jsonDoc["hum_min"] = humMin;
                    jsonDoc["hum_max"] = humMax;
                    jsonDoc["hum_avg"] = round((humSum / statsCount) * 100.0) / 100.0;

                    jsonDoc["pres_min"] = presMin;
                    jsonDoc["pres_max"] = presMax;
                    jsonDoc["pres_avg"] = round((presSum / statsCount) * 100.0) / 100.0;
                }
                String dataPayload;
                serializeJson(jsonDoc, dataPayload);
                
                mqttClient.publish(("sensornet/data/" + deviceId + "/").c_str(), dataPayload.c_str());
                lastUploadTime = millis(); // Trigger upload icon

                JsonDocument statusDoc;
                statusDoc["rssi"] = lastRssi;
                statusDoc["uptime"] = millis() / 1000;
                String statusPayload;
                serializeJson(statusDoc, statusPayload);

                mqttClient.publish(("sensornet/status/" + deviceId + "/").c_str(), statusPayload.c_str());

                drawNormalScreen(lastTemp, lastHum, lastPres, lastRssi);
            }
            break;
        }
    }
    
    delay(100); 
}
