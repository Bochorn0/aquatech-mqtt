// ================== AQUATECH PRESSURE CONTROLLER - VERSIÓN COMPLETA ==================
#include <SPI.h>
#include <Ethernet.h>
#include <TFT_eSPI.h>
#include "BluetoothSerial.h"
#include <driver/adc.h>
#include <time.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include <esp_timer.h>

// ================== BLUETOOTH SERIAL ==================
BluetoothSerial SerialBT;

// ================== CONFIGURACIÓN BÁSICA ==================
unsigned long updateControllerTime = 60000; // 60 segundos (1 minuto)
unsigned long loopTime = 60000;             // 60 segundos (1 minuto) para envío de datos
unsigned long lastUpdateController = 0;
unsigned long lastLoop = 0;
unsigned long lastTimerUpdate = 0;
unsigned long lastSyncTime = 0;

// Control de logs y mock data
#define ENABLE_SERIAL_LOGS false  // Cambiar a true para debug
#define USE_MOCK_DATA true        // Cambiar a false para usar sensores reales

// Modo de desarrollo - controla el envío de datos
bool devMode = true;  // true = envía datos al servidor, false = solo lectura local

// ================== ETHERNET CONFIGURATION ==================
#define PIN_MISO 17
#define PIN_MOSI 13
#define PIN_SCK  14
#define PIN_CS   26
#define PIN_RST  22
#define PIN_INT  34

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress serverIp(164, 92, 95, 176);
const int serverPort = 3009;

EthernetClient client;

// ================== PIN DEFINITIONS ==================
const int PRESSURE_PIN_1 = 35;  // Sensor de presión Entrada
const int PRESSURE_PIN_2 = 33;  // Sensor de presión Salida
const int RELAY_PIN = 16;       // Relay controlado por diferencia de presión

// ================== SENSOR CONFIGURATION ==================
const int N_SAMPLES = 10;                      // Promedio de lecturas
const float RAW_TO_PSI = 0.0267;               // Calibrado: Ajustado para 60 psi real (49->60: 0.0218 * 1.2245)
const float PSI_TO_KPA = 6.89476;              // Conversión PSI a kPa
const float PRESSURE_DIFF_THRESHOLD = 10.0f;   // Diferencia mínima para activar relay (psi)

// Validación de rango
const float MIN_PSI = 1.0;                     // Presión mínima válida
const float MAX_PSI = 120.0;                   // Presión máxima válida (aumentado para tolerar picos)
const float MIN_VOLTAGE = 0.05;                // Voltaje mínimo para detectar sensor conectado
const float MAX_VOLTAGE = 3.35;                // Voltaje máximo esperado (con tolerancia sobre 3.3V)

// ================== SYSTEM STATE ==================
// Entrada (Pin 35)
float pressure1_psi = 0.0f;
float voltage1 = 0.0f;
int raw1 = 0;
bool valid1 = false;
bool connected1 = false;

// Salida (Pin 33)
float pressure2_psi = 0.0f;
float voltage2 = 0.0f;
int raw2 = 0;
bool valid2 = false;
bool connected2 = false;

// Diferencia de presión y estado del relay
float PRESSURE_DIFFERENCE = 0.0f;
bool RELAY_STATE = false;

bool CURRENT_CONNECTION_STATE = false;
bool POWER_ON = true;
String product_name = "AquaTech Pressure Monitor";
bool reset_pending = false;
unsigned long flush_time = 0;

// ================== NETWORK STATE ==================
bool connectedEthernet = false;
bool connectedWiFi = false;
String deviceIP = "0.0.0.0";
String jwtToken = "";
String productId = "--";  // Se actualizará desde el servidor
String CONNECTION_TYPE = "Sin Red";
const char* controllerId = "690b81f46e38ab373c4818c3";

// URLs para HTTPClient (WiFi)
const char* loginUrl = "http://164.92.95.176:3009/api/v1.0/auth/login";
const char* controllerUrl = "http://164.92.95.176:3009/api/v1.0/controllers/690b81f46e38ab373c4818c3";
const char* dataUrl = "http://164.92.95.176:3009/api/v1.0/products/componentInput";

// ================== PUMP ANIMATION ==================
float pumpRotation = 0.0;
float currentPumpSpeed = 0.15;
unsigned long lastPumpUpdate = 0;

// ================== DISPLAY STATE TRACKING ==================
static bool lastPowerState = true;
static bool lastConnectionState = false;
static String lastConnectionType = "";
static float lastPressure1PSI = -999;
static float lastPressure2PSI = -999;
static bool lastRelayState = false;
static String lastProductName = "___FORCE_UPDATE___";
static bool displayInitialized = false;

// ================== DISPLAY ==================
TFT_eSPI tft = TFT_eSPI();

// ================== FUNCTION DECLARATIONS ==================
bool tryConnectSavedWiFi(unsigned long timeoutMs);
void connectEthernet();
void connectWiFi(const char* ssid, const char* password, unsigned long timeoutMs);
void handleBluetoothWiFi();
void updateNetworkStatus();
void updatePumpIcon();
void drawStatusMessage(String message, uint16_t color);
void clearStatusMessage();
void drawBluetoothIcon(int x, int y);
void updateNetworkIconDuringConnection(String connectionType);
void drawNetworkIconWithLoading(int x, int y, String type);
bool testServerConnectivity();
void obtenerToken();
String getControllerById();
void enviarDatos(const char* productIdParam, float pressure1PSI, float pressure2PSI, float pressureDiff, float temperature);
bool patchControllerResetPending();

// ================== UTILITY FUNCTIONS ==================
void logger(const char* titulo, const char* formato, ...) {
  #if ENABLE_SERIAL_LOGS
  char buffer[256];
  va_list args;
  va_start(args, formato);
  vsnprintf(buffer, sizeof(buffer), formato, args);
  va_end(args);

  Serial.printf("[%s] %s\n", titulo, buffer);
  SerialBT.printf("[%s] %s\n", titulo, buffer);
  #endif
}

// ================== SENSOR FUNCTIONS ==================
float readAvgRaw(int pin, int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(2);
  }
  return (float)sum / samples;
}

void readPressureSensors() {
  #if USE_MOCK_DATA
    // ===== DATOS MOCK PARA PRUEBAS =====
    // Generar valores aleatorios en los rangos especificados
    pressure1_psi = 40.0 + (random(0, 100) / 10.0);  // 40.0 - 50.0 psi
    pressure2_psi = 35.0 + (random(0, 100) / 10.0);  // 35.0 - 45.0 psi
    
    // Simular valores raw y voltage basados en los psi
    raw1 = pressure1_psi / RAW_TO_PSI;
    voltage1 = raw1 * (3.3 / 4095.0);
    
    raw2 = pressure2_psi / RAW_TO_PSI;
    voltage2 = raw2 * (3.3 / 4095.0);
    
    // Mock data siempre está "conectado" y "válido"
    connected1 = true;
    valid1 = true;
    connected2 = true;
    valid2 = true;
    
    // Calcular diferencia
    PRESSURE_DIFFERENCE = abs(pressure1_psi - pressure2_psi);
    
  #else
    // ===== LECTURA REAL DE SENSORES =====
    // Leer Pin 35
    raw1 = readAvgRaw(PRESSURE_PIN_1, N_SAMPLES);
    voltage1 = raw1 * (3.3 / 4095.0);
    pressure1_psi = raw1 * RAW_TO_PSI;
    
    // Detectar si sensor 1 está conectado
    connected1 = (voltage1 >= MIN_VOLTAGE);
    
    // Validar rango sensor 1 (solo si está conectado)
    if (connected1) {
      valid1 = (pressure1_psi >= MIN_PSI && pressure1_psi <= MAX_PSI);
    } else {
      valid1 = false;
      pressure1_psi = 0.0;
    }
    
    // Leer Pin 33
    raw2 = readAvgRaw(PRESSURE_PIN_2, N_SAMPLES);
    voltage2 = raw2 * (3.3 / 4095.0);
    pressure2_psi = raw2 * RAW_TO_PSI;
    
    // Detectar si sensor 2 está conectado
    connected2 = (voltage2 >= MIN_VOLTAGE);
    
    // Validar rango sensor 2 (solo si está conectado)
    if (connected2) {
      valid2 = (pressure2_psi >= MIN_PSI && pressure2_psi <= MAX_PSI);
    } else {
      valid2 = false;
      pressure2_psi = 0.0;
    }
    
    // Calcular diferencia de presión (solo si ambos están conectados y válidos)
    if (connected1 && connected2 && valid1 && valid2) {
      PRESSURE_DIFFERENCE = abs(pressure1_psi - pressure2_psi);
    } else {
      PRESSURE_DIFFERENCE = 0.0;
    }
  #endif
}

// ================== RELAY CONTROL ==================
void updateRelayControl() {
  bool previousRelayState = RELAY_STATE;
  
  // Activar relay solo si ambos sensores están conectados, válidos y diferencia >= 10 psi
  if (connected1 && connected2 && valid1 && valid2 && PRESSURE_DIFFERENCE >= PRESSURE_DIFF_THRESHOLD) {
    if (!RELAY_STATE) {
      RELAY_STATE = true;
      digitalWrite(RELAY_PIN, HIGH);  // Activar relay (invertido para módulo relay)
      #if ENABLE_SERIAL_LOGS
      Serial.printf("[RELAY] ACTIVADO - Diferencia %d psi\n", (int)PRESSURE_DIFFERENCE);
      #endif
    }
  } else {
    if (RELAY_STATE) {
      RELAY_STATE = false;
      digitalWrite(RELAY_PIN, LOW);  // Desactivar relay (invertido para módulo relay)
      #if ENABLE_SERIAL_LOGS
      Serial.printf("[RELAY] DESACTIVADO\n");
      #endif
    }
  }
}

// ================== SYSTEM LOGIC ==================
void updateSystemLogic() {
  updateRelayControl();
  updateNetworkStatus();
}

// ================== NETWORK FUNCTIONS ==================
void initializeNetwork() {
  drawCompleteDisplay();
  drawStatusMessage("Iniciando AquaTech Controller...", TFT_BLUE);
  delay(1000);
  
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(200);
  digitalWrite(PIN_RST, HIGH);
  delay(200);
  pinMode(PIN_INT, INPUT);
  
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);
  
  drawStatusMessage("Intentando conectar usando cable...", TFT_BLUE);
  updateNetworkIconDuringConnection("Cable");
  
  connectEthernet();
  
  if (connectedEthernet) {
    drawStatusMessage("Cable OK - IP: " + deviceIP+ "      ", TFT_GREEN);
    delay(2000);
  }
  
  if (!connectedEthernet) {
    drawStatusMessage("Intentando WiFi...", TFT_ORANGE);
    updateNetworkIconDuringConnection("WiFi");
    
    if (!tryConnectSavedWiFi(3000)) {
      drawStatusMessage("Esperando BT...", TFT_RED);
      updateNetworkIconDuringConnection("Bluetooth");
      deviceIP = "0.0.0.0";
      CURRENT_CONNECTION_STATE = false;
    } else {
      drawStatusMessage("WiFi OK - IP: " + deviceIP, TFT_GREEN);
      delay(2000);
    }
  }
  
  updateNetworkStatus();
  delay(3000);
  clearStatusMessage();
}

// ================== API FUNCTIONS (Simplificadas - usar las de componente_presion.ino si necesitas) ==================
bool testServerConnectivity() {
  if (!connectedEthernet && !connectedWiFi) return false;
  
  if (connectedWiFi) {
    HTTPClient http;
    http.begin("http://164.92.95.176:3009/");
    http.setTimeout(3000);
    int httpCode = http.GET();
    http.end();
    return (httpCode > 0);
  } else if (connectedEthernet) {
    EthernetClient testClient;
    testClient.setTimeout(2000);
    if (testClient.connect(serverIp, serverPort)) {
      testClient.stop();
      return true;
    }
  }
  return false;
}

void obtenerToken() {
  if (!connectedEthernet && !connectedWiFi) {
    return;
  }
  
  String loginPayload = "{\"email\":\"esp32@lcc.com.mx\",\"password\":\"Esp32*\"}";
  String response = "";
  
  if (connectedWiFi) {
    HTTPClient http;
    http.begin(loginUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    
    int httpCode = http.POST(loginPayload);
    
    if (httpCode > 0) {
      response = http.getString();
    }
    http.end();
    
  } else if (connectedEthernet) {
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      return;
    }
    
    localClient.print("POST /api/v1.0/auth/login HTTP/1.1\r\n");
    localClient.print("Host: 164.92.95.176\r\n");
    localClient.print("Content-Type: application/json\r\n");
    localClient.print("Content-Length: ");
    localClient.println(loginPayload.length());
    localClient.print("Connection: close\r\n\r\n");
    localClient.print(loginPayload);

    unsigned long timeout = millis();
    while (millis() - timeout < 5000) {
      while (localClient.available()) {
        char c = localClient.read();
        response += c;
        timeout = millis();
      }
    }
    localClient.stop();
    
    int bodyIndex = response.indexOf("\r\n\r\n");
    if (bodyIndex != -1) {
      response = response.substring(bodyIndex + 4);
    }
  }

  if (response.length() > 0) {
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
      if (doc.containsKey("token")) {
        jwtToken = doc["token"].as<String>();
        return;
      } else if (doc.containsKey("access_token")) {
        jwtToken = doc["access_token"].as<String>();
        return;
      }
    }

    int tokenIndex = response.indexOf("\"token\":\"");
    if (tokenIndex != -1) {
      int start = tokenIndex + 9;
      int end = response.indexOf("\"", start);
      jwtToken = response.substring(start, end);
    }
  }
}

String getControllerById() {
  if (!connectedEthernet && !connectedWiFi) {
    return "";
  }
  
  if (jwtToken == "") {
    obtenerToken();
    delay(500);
    if (jwtToken == "") {
      return "";
    }
  }
  
  String response = "";
  String json = "";
  
  if (connectedWiFi) {
    HTTPClient http;
    http.begin(controllerUrl);
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);
    
    int httpCode = http.GET();
    
    if (httpCode > 0) {
      response = http.getString();
      json = response;
    }
    http.end();
    
  } else if (connectedEthernet) {
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      return "";
    }

    String request = "";
    request += "GET /api/v1.0/controllers/" + String(controllerId) + " HTTP/1.1\r\n";
    request += "Host: 164.92.95.176\r\n";
    request += "Authorization: Bearer " + jwtToken + "\r\n";
    request += "Connection: close\r\n\r\n";

    localClient.print(request);

    unsigned long timeout = millis();
    while (localClient.connected() && millis() - timeout < 5000) {
      while (localClient.available()) response += (char)localClient.read();
    }
    localClient.stop();

    int jsonStart = response.indexOf("{");
    if (jsonStart == -1) {
      return "";
    }
    json = response.substring(jsonStart);
  }

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.println("[CONFIG] Error parseando JSON del servidor");
    return json;
  }
  
  // Mostrar JSON recibido completo para debug
  Serial.println("\n========== RESPUESTA DEL SERVIDOR ==========");
  Serial.println(json);
  Serial.println("============================================\n");
  
  // Listar todos los campos disponibles
  Serial.println("[CONFIG] Campos encontrados en JSON:");
  if (doc.containsKey("_id")) Serial.println("  - _id");
  if (doc.containsKey("name")) Serial.println("  - name");
  if (doc.containsKey("product")) Serial.println("  - product ✓ (productId)");
  if (doc.containsKey("productId")) Serial.println("  - productId ✓");
  if (doc.containsKey("product_name")) Serial.println("  - product_name");
  if (doc.containsKey("loop_time")) Serial.println("  - loop_time");
  if (doc.containsKey("update_controller_time")) Serial.println("  - update_controller_time");
  if (doc.containsKey("reset_pending")) Serial.println("  - reset_pending");
  if (doc.containsKey("flush_time")) Serial.println("  - flush_time");

  if (doc.containsKey("update_controller_time")) {
    unsigned long newUpdateTime = doc["update_controller_time"].as<unsigned long>();
    if (newUpdateTime >= 10000 && newUpdateTime != updateControllerTime) {
      updateControllerTime = newUpdateTime;
      Serial.printf("[CONFIG] update_controller_time actualizado: %lu ms (%lu seg)\n", updateControllerTime, updateControllerTime/1000);
    }
  }

  if (doc.containsKey("loop_time")) {
    unsigned long newLoopTime = doc["loop_time"].as<unsigned long>();
    if (newLoopTime >= 500 && newLoopTime != loopTime) {
      loopTime = newLoopTime;
      Serial.printf("[CONFIG] loop_time actualizado: %lu ms (%lu seg)\n", loopTime, loopTime/1000);
    }
  }

  // Buscar productId - puede venir como "product" o "productId"
  String newProductId = "";
  if (doc.containsKey("product")) {
    newProductId = doc["product"].as<String>();
    Serial.printf("[CONFIG] product (productId) recibido del servidor: '%s'\n", newProductId.c_str());
  } else if (doc.containsKey("productId")) {
    newProductId = doc["productId"].as<String>();
    Serial.printf("[CONFIG] productId recibido del servidor: '%s'\n", newProductId.c_str());
  }
  
  if (newProductId.length() > 0) {
    if (newProductId != productId) {
      String oldProductId = productId;
      productId = newProductId;
      Serial.printf("[CONFIG] ✓ productId actualizado: '%s' -> '%s'\n", oldProductId.c_str(), productId.c_str());
    } else {
      Serial.println("[CONFIG] productId sin cambios (mismo valor)");
    }
  } else {
    Serial.println("[CONFIG] ❌ ERROR: No se encontro 'product' ni 'productId' en respuesta del servidor");
    Serial.println("[CONFIG] El productId actual sigue siendo: " + productId);
  }

  if (doc.containsKey("product_name")) {
    String newProductName = doc["product_name"].as<String>();
    if (newProductName != product_name) {
      product_name = newProductName;
      Serial.printf("[CONFIG] product_name actualizado: %s\n", product_name.c_str());
    }
  }

  if (doc.containsKey("reset_pending")) {
    bool newResetPending = doc["reset_pending"].as<bool>();
    if (newResetPending != reset_pending) {
      reset_pending = newResetPending;
    }
  }

  if (doc.containsKey("flush_time")) {
    unsigned long newFlushTime = doc["flush_time"].as<unsigned long>();
    if (newFlushTime != flush_time) {
      flush_time = newFlushTime;
    }
  }

  if (response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
    jwtToken = "";
  }

  return json;
}

void enviarDatos(const char* productIdParam, float pressure1PSI, float pressure2PSI, float pressureDiff, float temperature) {
  if (!connectedEthernet && !connectedWiFi) {
    return;
  }
  
  if (jwtToken == "") {
    obtenerToken();
    delay(500);
    if (jwtToken == "") {
      return;
    }
  }

  String jsonPayload = "";
  jsonPayload += "{";
  jsonPayload += "\"productId\":\"" + String(productIdParam) + "\",";
  jsonPayload += "\"pressure_valve1_psi\":" + String(pressure1PSI, 2) + ",";
  jsonPayload += "\"pressure_valve2_psi\":" + String(pressure2PSI, 2) + ",";
  jsonPayload += "\"pressure_difference_psi\":" + String(pressureDiff, 2) + ",";
  jsonPayload += "\"relay_state\":" + String(RELAY_STATE ? "true" : "false") + ",";
  jsonPayload += "\"temperature\":" + String(temperature, 2) + ",";
  jsonPayload += "\"timestamp\":\"" + String(millis()) + "\"";
  jsonPayload += "}";

  if (connectedWiFi) {
    HTTPClient http;
    http.begin(dataUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);
    
    int httpCode = http.POST(jsonPayload);
    
    #if ENABLE_SERIAL_LOGS
    if (httpCode > 0) {
      Serial.printf("[API] Datos enviados via WiFi, código: %d\n", httpCode);
    } else {
      Serial.printf("[API] Error enviando datos via WiFi, código: %d\n", httpCode);
    }
    #endif
    
    http.end();
    
  } else if (connectedEthernet) {
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      return;
    }

    String request = "";
    request += "POST /api/v1.0/products/componentInput HTTP/1.1\r\n";
    request += "Host: 164.92.95.176\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Authorization: Bearer " + jwtToken + "\r\n";
    request += "Content-Length: " + String(jsonPayload.length()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += jsonPayload;

    localClient.print(request);

    String response = "";
    unsigned long timeout = millis();
    while (localClient.connected() && millis() - timeout < 5000) {
      while (localClient.available()) {
        char c = localClient.read();
        response += c;
        timeout = millis();
      }
    }
    localClient.stop();

    #if ENABLE_SERIAL_LOGS
    if (response.length() > 0) {
      Serial.println("[API] Datos enviados via Ethernet");
    }
    #endif
  }

  // Renovar token preventivamente cada 5 minutos
  static unsigned long lastTokenCheck = 0;
  if (millis() - lastTokenCheck > 300000) {
    jwtToken = "";
    lastTokenCheck = millis();
  }
}

bool patchControllerResetPending() {
  if (!connectedEthernet && !connectedWiFi) {
    return false;
  }
  
  if (jwtToken == "") {
    obtenerToken();
    delay(500);
    if (jwtToken == "") {
      return false;
    }
  }
  
  String patchPayload = "{\"reset_pending\":false}";
  String response = "";
  
  if (connectedWiFi) {
    HTTPClient http;
    http.begin(controllerUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);
    
    int httpCode = http.PATCH(patchPayload);
    
    if (httpCode > 0) {
      response = http.getString();
    }
    http.end();
    
  } else if (connectedEthernet) {
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      return false;
    }
    
    localClient.print("PATCH /api/v1.0/controllers/" + String(controllerId) + " HTTP/1.1\r\n");
    localClient.print("Host: 164.92.95.176\r\n");
    localClient.print("Content-Type: application/json\r\n");
    localClient.print("Authorization: Bearer " + jwtToken + "\r\n");
    localClient.print("Content-Length: ");
    localClient.println(patchPayload.length());
    localClient.print("Connection: close\r\n\r\n");
    localClient.print(patchPayload);

    unsigned long timeout = millis();
    while (millis() - timeout < 5000) {
      while (localClient.available()) {
        char c = localClient.read();
        response += c;
        timeout = millis();
      }
    }
    localClient.stop();
  }

  return (response.length() > 0);
}

void connectEthernet() {
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(200);
  digitalWrite(PIN_RST, HIGH);
  delay(200);
  pinMode(PIN_INT, INPUT);
  
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);
  
  int result = Ethernet.begin(mac);
  
  if (result == 0) {
    connectedEthernet = false;
    deviceIP = "0.0.0.0";
    return;
  }
  
  IPAddress ip = Ethernet.localIP();
  if (ip == INADDR_NONE) {
    connectedEthernet = false;
    deviceIP = "0.0.0.0";
    return;
  }
  
  deviceIP = ip.toString();
  connectedEthernet = true;
  CONNECTION_TYPE = "Cable";
  CURRENT_CONNECTION_STATE = true;
}

bool tryConnectSavedWiFi(unsigned long timeoutMs = 5000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    deviceIP = WiFi.localIP().toString();
    connectedWiFi = true;
    CONNECTION_TYPE = "WiFi";
    CURRENT_CONNECTION_STATE = true;
    return true;
  }
  
  connectedWiFi = false;
  return false;
}

void connectWiFi(const char* ssid, const char* password, unsigned long timeoutMs = 5000) {
  WiFi.mode(WIFI_STA);
  
  WiFi.begin(ssid, password);
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    deviceIP = WiFi.localIP().toString();
    connectedWiFi = true;
    CONNECTION_TYPE = "WiFi";
    CURRENT_CONNECTION_STATE = true;
  } else {
    connectedWiFi = false;
  }
}

void handleBluetoothWiFi() {
  static String credenciales = "";
  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\r') continue;
    if (c == '\n') {
      credenciales.trim();
      if (credenciales.length() == 0) {
        credenciales = "";
        return;
      }
      
      if (credenciales.equalsIgnoreCase("CLEAR_WIFI")) {
        WiFi.disconnect(true, true);
        delay(500);
        ESP.restart();
      }
      
      int comaIndex = credenciales.indexOf(',');
      if (comaIndex > 0) {
        String ssid = credenciales.substring(0, comaIndex);
        String password = credenciales.substring(comaIndex + 1);
        
        ssid.trim();
        password.trim();
        
        WiFi.mode(WIFI_STA);
        WiFi.persistent(true);
        WiFi.begin(ssid.c_str(), password.c_str());
        
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
          delay(100);
        }
        
        if (WiFi.status() == WL_CONNECTED) {
          deviceIP = WiFi.localIP().toString();
          connectedWiFi = true;
          CONNECTION_TYPE = "WiFi";
          CURRENT_CONNECTION_STATE = true;
          delay(300);
          ESP.restart();
        } else {
          WiFi.persistent(false);
        }
      } else {
        SerialBT.println("[ERROR] Formato inválido. Usa: NOMBRE_RED,PASSWORD_RED");
      }
      credenciales = "";
    } else {
      credenciales += c;
      if (credenciales.length() > 200) credenciales = credenciales.substring(0, 200);
    }
  }
}

void updateNetworkStatus() {
  if (connectedEthernet || connectedWiFi) {
    CURRENT_CONNECTION_STATE = true;
    deviceIP = connectedEthernet ? Ethernet.localIP().toString() : WiFi.localIP().toString();
  } else {
    CURRENT_CONNECTION_STATE = false;
    deviceIP = "0.0.0.0";
    CONNECTION_TYPE = "Sin Red";
  }
}

void updatePumpIcon() {
  int screenWidth = tft.width();
  int iconWidth = 60;
  int totalIconWidth = iconWidth * 3;
  int spacing = (screenWidth - totalIconWidth) / 4;
  
  if (spacing < 5) {
    spacing = 5;
    iconWidth = (screenWidth - spacing * 4) / 3;
  }
  
  int iconY = 10;
  int pumpX = spacing + (iconWidth + spacing) * 1;
  
  clearArea(pumpX, iconY, iconWidth, 80);
  drawPumpIcon(pumpX, iconY, RELAY_STATE);
}

// ================== DISPLAY FUNCTIONS ==================
void clearArea(int x, int y, int w, int h) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x + w > tft.width()) w = tft.width() - x;
  if (y + h > tft.height()) h = tft.height() - y;
  
  tft.fillRect(x, y, w, h, TFT_WHITE);
}

void drawPowerIcon(int x, int y, bool state) {
  int radius = 25;
  uint16_t color = state ? 0xF800 : TFT_DARKGREY;
  
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  tft.drawLine(x + radius, y + 8, x + radius, y + radius + 5, TFT_WHITE);
  tft.drawCircle(x + radius, y + 15, 8, TFT_WHITE);
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 8, y + 60);
  tft.print("Power");
}

void drawPressureIcon(int x, int y) {
  int radius = 25;
  uint16_t color = TFT_ORANGE;
  
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  tft.drawCircle(x + radius, y + radius, 18, TFT_WHITE);
  tft.drawCircle(x + radius, y + radius, 17, TFT_WHITE);
  
  int needleX = x + radius + 10 * cos(0.785);
  int needleY = y + radius + 10 * sin(0.785);
  tft.drawLine(x + radius, y + radius, needleX, needleY, TFT_WHITE);
  tft.drawLine(x + radius + 1, y + radius, needleX + 1, needleY, TFT_WHITE);
  
  tft.fillCircle(x + radius, y + radius, 3, TFT_WHITE);
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 2, y + 60);
  tft.print("Presion");
}

void drawPumpIcon(int x, int y, bool state) {
  int radius = 25;
  uint16_t color = state ? TFT_ORANGE : TFT_DARKGREY;
  
  tft.fillCircle(x + radius, y + radius, radius + 2, TFT_WHITE);
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  if (state && millis() - lastPumpUpdate > 500) {
    currentPumpSpeed = 0.3;
    pumpRotation += currentPumpSpeed;
    if (pumpRotation > 6.28) pumpRotation = 0;
    lastPumpUpdate = millis();
  } else if (!state) {
    pumpRotation = 0;
  }
  
  float baseAngle = state ? pumpRotation : 0;
  for (int i = 0; i < 3; i++) {
    float angle = baseAngle + i * 2.094;
    
    int x1 = x + radius + 12 * cos(angle);
    int y1 = y + radius + 12 * sin(angle);
    
    tft.drawLine(x + radius, y + radius, x1, y1, TFT_WHITE);
    tft.drawLine(x + radius + 1, y + radius, x1 + 1, y1, TFT_WHITE);
    tft.drawLine(x + radius, y + radius + 1, x1, y1 + 1, TFT_WHITE);
    tft.fillCircle(x1, y1, 1, TFT_WHITE);
  }
  
  tft.fillCircle(x + radius, y + radius, 4, TFT_BLACK);
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x - 28, y + 60);
  tft.print("Retro Lavado");
}

void drawNetworkIcon(int x, int y, bool online, String type) {
  int radius = 25;
  uint16_t color;
  String label = "Red";
  
  if (!online) {
    color = TFT_DARKGREY;
    label = "Red";
  } else if (type == "WiFi") {
    color = 0x07E0;
    label = "WiFi";
  } else if (type == "Cable") {
    color = 0x07FF;
    label = "Cable";
  } else {
    color = TFT_DARKGREY;
    label = "Red";
  }
  
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  if (online) {
    if (type == "WiFi") {
      tft.drawCircle(x + radius, y + radius + 5, 6, TFT_BLACK);
      tft.drawCircle(x + radius, y + radius + 5, 10, TFT_BLACK);
      tft.drawCircle(x + radius, y + radius + 5, 14, TFT_BLACK);
      tft.fillCircle(x + radius, y + radius + 5, 2, TFT_BLACK);
    } else if (type == "Cable") {
      tft.fillRect(x + radius - 8, y + radius - 3, 16, 6, TFT_BLACK);
      tft.drawLine(x + radius - 10, y + radius, x + radius - 8, y + radius, TFT_BLACK);
      tft.drawLine(x + radius + 8, y + radius, x + radius + 10, y + radius, TFT_BLACK);
    }
  } else {
    tft.drawLine(x + radius - 6, y + radius - 6, x + radius + 6, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius + 6, y + radius - 6, x + radius - 6, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius - 5, y + radius - 6, x + radius + 7, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius + 5, y + radius - 6, x + radius - 7, y + radius + 6, TFT_BLACK);
  }
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 12, y + 60);
  tft.print(label);
  
  if (online && type == "Cable" && deviceIP != "0.0.0.0") {
    tft.fillRect(x, y + 75, 60, 15, TFT_WHITE);
    tft.setTextSize(1);
    tft.setTextColor(TFT_BLUE, TFT_WHITE);
    tft.setCursor(x + 5, y + 80);
    tft.print(deviceIP);
  }
}

void drawTimeCounter(unsigned long seconds) {
  int x = 10;
  int y = 280;
  int w = 300;
  int h = 25;
  
  clearArea(x, y - 5, w, h + 10);
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLUE, TFT_WHITE);
  tft.setCursor(x, y);
  
  tft.printf("Actualizado hace %lu seg", seconds);
}

void drawStatusMessage(String message, uint16_t color) {
  int x = 10;
  int y = 100;
  int w = 300;
  int h = 25;
  
  clearArea(x, y - 5, w, h + 10);
  
  tft.setTextSize(2);
  tft.setTextColor(color, TFT_WHITE);
  tft.setCursor(x, y);
  tft.print(message);
}

void clearStatusMessage() {
  int x = 10;
  int y = 100;
  int w = 300;
  int h = 25;
  
  clearArea(x, y - 5, w, h + 10);
}

void drawBluetoothIcon(int x, int y) {
  int radius = 15;
  uint16_t color = TFT_BLUE;
  
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  tft.drawLine(x + radius - 3, y + radius - 8, x + radius + 3, y + radius, TFT_WHITE);
  tft.drawLine(x + radius + 3, y + radius, x + radius - 3, y + radius + 8, TFT_WHITE);
  tft.drawLine(x + radius - 3, y + radius - 8, x + radius - 3, y + radius + 8, TFT_WHITE);
  tft.drawLine(x + radius + 3, y + radius - 4, x + radius + 3, y + radius + 4, TFT_WHITE);
  
  tft.setTextSize(1);
  tft.setTextColor(TFT_BLUE, TFT_WHITE);
  tft.setCursor(x + 2, y + 35);
  tft.print("BT");
}

void updateNetworkIconDuringConnection(String connectionType) {
  int screenWidth = tft.width();
  int iconWidth = 60;
  int totalIconWidth = iconWidth * 3;
  int spacing = (screenWidth - totalIconWidth) / 4;
  
  if (spacing < 5) {
    spacing = 5;
    iconWidth = (screenWidth - spacing * 4) / 3;
  }
  
  int iconY = 10;
  int networkX = spacing + (iconWidth + spacing) * 2;
  
  clearArea(networkX, iconY, iconWidth, 60);
  
  if (connectionType == "Cable") {
    drawNetworkIconWithLoading(networkX, iconY, "Cable");
  } else if (connectionType == "WiFi") {
    drawNetworkIconWithLoading(networkX, iconY, "WiFi");
  } else if (connectionType == "Bluetooth") {
    drawBluetoothIcon(networkX + 15, iconY + 15);
  }
}

void drawNetworkIconWithLoading(int x, int y, String type) {
  int radius = 25;
  int iconWidth = 60;
  uint16_t color;
  String label;
  
  if (type == "WiFi") {
    color = 0x07E0;
    label = "WiFi";
  } else {
    color = 0x07FF;
    label = "Cable";
  }
  
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  if (type == "WiFi") {
    tft.drawCircle(x + radius, y + radius + 5, 6, TFT_BLACK);
    tft.drawCircle(x + radius, y + radius + 5, 10, TFT_BLACK);
    tft.drawCircle(x + radius, y + radius + 5, 14, TFT_BLACK);
    tft.fillCircle(x + radius, y + radius + 5, 2, TFT_BLACK);
  } else {
    tft.fillRect(x + radius - 8, y + radius - 3, 16, 6, TFT_BLACK);
    tft.drawLine(x + radius - 10, y + radius, x + radius - 8, y + radius, TFT_BLACK);
    tft.drawLine(x + radius + 8, y + radius, x + radius + 10, y + radius, TFT_BLACK);
  }
  
  static unsigned long lastLoadingUpdate = 0;
  static int loadingDots = 0;
  
  if (millis() - lastLoadingUpdate > 500) {
    loadingDots = (loadingDots + 1) % 4;
    lastLoadingUpdate = millis();
  }
  
  tft.fillRect(x + 5, y + 50, iconWidth - 10, 15, TFT_WHITE);
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 12, y + 60);
  tft.print(label);
  
  for (int i = 0; i < loadingDots; i++) {
    tft.fillCircle(x + radius - 6 + i * 4, y + radius + 20, 2, TFT_BLACK);
  }
}

void drawCompleteDisplay() {
  tft.fillScreen(TFT_WHITE);
  
  int screenWidth = tft.width();
  int iconWidth = 60;
  int totalIconWidth = iconWidth * 3;
  int spacing = (screenWidth - totalIconWidth) / 4;
  
  if (spacing < 5) {
    spacing = 5;
    iconWidth = (screenWidth - spacing * 4) / 3;
  }
  
  int iconY = 10;
  int currentX = spacing;
  
  drawPowerIcon(currentX, iconY, POWER_ON);
  currentX += iconWidth + spacing;
  
  drawPumpIcon(currentX, iconY, RELAY_STATE);
  currentX += iconWidth + spacing;
  
  drawNetworkIcon(currentX, iconY, CURRENT_CONNECTION_STATE, CONNECTION_TYPE);
  
  // Dibujar etiquetas estáticas
  int startY = 130;
  int lineH = 35;
  int avgX = screenWidth * 0.75;
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
  tft.setCursor(10, startY);
  tft.print("Entrada:");
  
  tft.setTextColor(0xF800, TFT_WHITE);
  tft.setCursor(10, startY + lineH);
  tft.print("Salida:");
  
  tft.setTextColor(TFT_BLUE, TFT_WHITE);
  tft.setCursor(10, startY + lineH * 2);
  tft.print("Diferencia:");
  
  tft.setTextColor(TFT_PURPLE, TFT_WHITE);
  tft.setCursor(10, startY + lineH * 3);
  tft.print("Producto: ");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.printf("%s", product_name.c_str());
  
  // Dibujar valores actuales
  updateDisplayValues();
  
  // Dibujar contador de tiempo
  unsigned long now = millis();
  unsigned long timeSinceLastUpdate = (now - lastUpdateController) / 1000;
  unsigned long displayTime = timeSinceLastUpdate % (updateControllerTime / 1000);
  drawTimeCounter(displayTime);
  lastSyncTime = displayTime;
  
  // Actualizar estado tracking
  lastPowerState = POWER_ON;
  lastConnectionState = CURRENT_CONNECTION_STATE;
  lastConnectionType = CONNECTION_TYPE;
  lastRelayState = RELAY_STATE;
  lastProductName = product_name;
}

void updateDisplayValues() {
  int startY = 130;
  int lineH = 35;
  int screenWidth = tft.width();
  int valueX = screenWidth * 0.42;
  int avgX = screenWidth * 0.75;
  
  // Actualizar valor de Válvula 1
  clearArea(valueX, startY, avgX - valueX - 10, 30);
  tft.setTextSize(2);
  if (!connected1) {
    tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
    tft.setCursor(valueX, startY);
    tft.print("0 psi");
  } else if (valid1) {
    tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
    tft.setCursor(valueX, startY);
    tft.printf("%d psi", (int)pressure1_psi);
  } else {
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.setCursor(valueX, startY);
    tft.print("ERROR");
  }
  lastPressure1PSI = pressure1_psi;
  
  // Actualizar valor de Válvula 2
  clearArea(valueX, startY + lineH, avgX - valueX - 10, 30);
  tft.setTextSize(2);
  if (!connected2) {
    tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
    tft.setCursor(valueX, startY + lineH);
    tft.print("0 psi");
  } else if (valid2) {
    tft.setTextColor(0xF800, TFT_WHITE);
    tft.setCursor(valueX, startY + lineH);
    tft.printf("%d psi", (int)pressure2_psi);
  } else {
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.setCursor(valueX, startY + lineH);
    tft.print("ERROR");
  }
  lastPressure2PSI = pressure2_psi;
  
  // Actualizar diferencia de presión
  clearArea(valueX, startY + lineH * 2, avgX - valueX - 10, 30);
  tft.setTextSize(2);
  if (connected1 && connected2 && valid1 && valid2) {
    uint16_t diffColor = (PRESSURE_DIFFERENCE >= PRESSURE_DIFF_THRESHOLD) ? TFT_RED : TFT_BLUE;
    tft.setTextColor(diffColor, TFT_WHITE);
    tft.setCursor(valueX, startY + lineH * 2);
    tft.printf("%d psi", (int)PRESSURE_DIFFERENCE);
  } else {
    tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
    tft.setCursor(valueX, startY + lineH * 2);
    tft.print("--");
  }
  
  // Actualizar product_name
  if (!displayInitialized || product_name != lastProductName) {
    clearArea(10, startY + lineH * 3, screenWidth - 20, 30);
    tft.setTextSize(2);
    tft.setTextColor(TFT_PURPLE, TFT_WHITE);
    tft.setCursor(10, startY + lineH * 3);
    tft.print("Producto: ");
    tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
    tft.printf("%s", product_name.c_str());
    lastProductName = product_name;
  }
  
  // Actualizar animación de bomba si está activa
  static unsigned long lastPumpAnimation = 0;
  if (RELAY_STATE && millis() - lastPumpAnimation > 500) {
    int screenWidth = tft.width();
    int iconWidth = 60;
    int spacing = (screenWidth - iconWidth * 3) / 4;
    if (spacing < 5) {
      spacing = 5;
      iconWidth = (screenWidth - spacing * 4) / 3;
    }
    int pumpX = spacing + (iconWidth + spacing) * 1;
    
    clearArea(pumpX, 10, iconWidth, 80);
    drawPumpIcon(pumpX, 10, RELAY_STATE);
    lastPumpAnimation = millis();
  }
  
  displayInitialized = true;
}

void updateDisplayTimer() {
  unsigned long now = millis();
  
  if (now - lastTimerUpdate >= 1000) {
    unsigned long timeSinceLastUpdate = (now - lastUpdateController) / 1000;
    unsigned long displayTime = timeSinceLastUpdate % (updateControllerTime / 1000);
    
    if (displayTime != lastSyncTime) {
      drawTimeCounter(displayTime);
      lastSyncTime = displayTime;
    }
    
    lastTimerUpdate = now;
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  SerialBT.begin("AquaTech_Pressure");
  
  Serial.println("\n=== AQUATECH PRESSURE MONITOR ===");
  
  #if ENABLE_SERIAL_LOGS
  Serial.println("Modo: DEBUG ACTIVO");
  #else
  Serial.println("Modo: Produccion (logs desactivados)");
  #endif
  
  #if USE_MOCK_DATA
  Serial.println("Datos: MOCK DATA (V1: 40-50 psi, V2: 35-45 psi)");
  #else
  Serial.println("Datos: SENSORES REALES");
  #endif
  
  Serial.printf("Frecuencia de lectura: %lu segundos\n", loopTime / 1000);
  Serial.printf("Envio de datos: %s\n", devMode ? "ACTIVADO" : "DESACTIVADO");
  
  // Configurar pines
  pinMode(PRESSURE_PIN_1, INPUT);
  pinMode(PRESSURE_PIN_2, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  
  // Configurar ADC
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(PRESSURE_PIN_1, ADC_11db);
  analogSetPinAttenuation(PRESSURE_PIN_2, ADC_11db);
  
  // Estado inicial del relay (desactivado)
  digitalWrite(RELAY_PIN, LOW);
  RELAY_STATE = false;
  
  // Inicializar pantalla
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  
  tft.writecommand(0x2A);
  tft.writedata(0x00); tft.writedata(0x00);
  tft.writedata(0x01); tft.writedata(0xDF);
  
  tft.writecommand(0x2B);
  tft.writedata(0x00); tft.writedata(0x00);
  tft.writedata(0x01); tft.writedata(0x3F);
  
  tft.writecommand(0x36);
  tft.writedata(0xE8);
  
  tft.fillScreen(TFT_WHITE);
  
  // Inicializar conectividad de red
  initializeNetwork();
  
  // Obtener configuración inicial solo si devMode está activo y hay conexión
  if (devMode && (connectedEthernet || connectedWiFi)) {
    getControllerById();
  }
  
  // Inicializar los tiempos para evitar envío inmediato
  lastLoop = millis();
  lastUpdateController = millis();
  
  Serial.println("\n>> Sistema listo. Esperando primer ciclo de lectura...\n");
}

// ================== MAIN LOOP ==================
void loop() {
  unsigned long now = millis();
  
  // ====== ACTUALIZAR ICONO DE LA BOMBA EN TIEMPO REAL ======
  static bool lastRelayStateLoop = false;
  if (RELAY_STATE != lastRelayStateLoop) {
    updatePumpIcon();
    lastRelayStateLoop = RELAY_STATE;
  }
  
  // ====== FLUJO 1: CONTADOR DE TIEMPO INDEPENDIENTE ======
  updateDisplayTimer();
  
  // ====== FLUJO 2: LECTURA DE SENSORES Y ENVÍO DE DATOS (CADA MINUTO) ======
  if (now - lastLoop >= loopTime) {
    // Leer sensores
    readPressureSensors();
    updateSystemLogic();
    
    // Actualizar pantalla
    updateDisplayValues();
    
    // Enviar datos solo si devMode está activado y hay conexión
    if (devMode && (connectedEthernet || connectedWiFi)) {
      #if !ENABLE_SERIAL_LOGS
      // Indicador simple de envío (solo en modo producción)
      Serial.printf("[%lu] Enviando datos con productId: %s\n", millis()/1000, productId.c_str());
      Serial.printf("       V1:%d V2:%d Diff:%d Relay:%s\n",
                    (int)pressure1_psi, (int)pressure2_psi, 
                    (int)PRESSURE_DIFFERENCE, RELAY_STATE ? "ON" : "OFF");
      #endif
      
      enviarDatos(productId.c_str(), pressure1_psi, pressure2_psi, PRESSURE_DIFFERENCE, 25.0);
      
      #if ENABLE_SERIAL_LOGS
      Serial.printf("[DATA] Enviado - V1: %d psi, V2: %d psi, Diff: %d psi, Relay: %s\n",
                    (int)pressure1_psi, (int)pressure2_psi, (int)PRESSURE_DIFFERENCE,
                    RELAY_STATE ? "ON" : "OFF");
      #endif
    } else if (!devMode) {
      // Modo devMode desactivado - solo lectura local
      #if !ENABLE_SERIAL_LOGS
      Serial.printf("[%lu] Lectura local - V1:%d V2:%d Diff:%d Relay:%s (devMode OFF)\n",
                    millis()/1000, (int)pressure1_psi, (int)pressure2_psi, 
                    (int)PRESSURE_DIFFERENCE, RELAY_STATE ? "ON" : "OFF");
      #endif
    }
    
    lastLoop = now;
  }
  
  // ====== ACTUALIZACIÓN VISUAL RÁPIDA (cada 5 segundos) ======
  static unsigned long lastQuickUpdate = 0;
  if (now - lastQuickUpdate >= 5000) {
    // Leer sensores para actualizar display sin enviar datos
    readPressureSensors();
    updateSystemLogic();
    updateDisplayValues();
    lastQuickUpdate = now;
  }
  
  // ====== CICLO COMPLETO: OBTENER CONFIGURACIÓN DEL SERVIDOR (CADA MINUTO) ======
  if (now - lastUpdateController >= updateControllerTime) {
    // Actualizar configuración del controller solo si devMode está activo y hay conexión
    if (devMode && (connectedEthernet || connectedWiFi)) {
      getControllerById();
      
      // Verificar si se solicita reinicio remoto
      if (reset_pending) {
        #if ENABLE_SERIAL_LOGS
        Serial.println("[RESET] Reinicio remoto solicitado");
        #endif
        patchControllerResetPending();
        delay(1000);
        ESP.restart();
      }
    }
    
    // Redibujar display completo
    drawCompleteDisplay();
    lastUpdateController = now;
  }
  
  // ====== MANEJO DE BLUETOOTH (solo si no hay red) ======
  if (!connectedEthernet && !connectedWiFi) {
    handleBluetoothWiFi();
  }
  
  delay(100);
}

