// ================== AQUATECH CONTROLLER - VERSIÓN CON PRESIÓN ==================
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
// Timing
unsigned long updateControllerTime = 10000; // 10 segundos
unsigned long loopTime = 1000;             // 1 segundo
unsigned long lastUpdateController = 0;
unsigned long lastLoop = 0;
unsigned long lastTimerUpdate = 0;
unsigned long lastSyncTime = 0;

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
const int PRESSURE_PIN_1 = 35;  // Sensor de presión Válvula 1
const int PRESSURE_PIN_2 = 32;  // Sensor de presión Válvula 2
const int RELAY_PIN = 16;       // Relay controlado por diferencia de presión

// ================== SENSOR CONFIGURATION ==================
// Configuración del sensor de presión
const int N_SAMPLES = 8;                    // Promedio de lecturas
const float RAW_TO_PSI = 0.02627;           // Calibrado a 19 psi
const float PSI_TO_KPA = 6.89476;           // Conversión PSI a kPa
const float ALPHA = 0.1f;                   // Filtro exponencial
const float PRESSURE_DIFF_THRESHOLD = 15.0f; // Diferencia mínima para activar relay (psi)

// ================== SYSTEM STATE ==================
// Válvula 1 (Pin 35)
float CURRENT_PRESSURE_1_RAW = 0.0f;
float CURRENT_PRESSURE_1_VOLTAGE = 0.0f;
float CURRENT_PRESSURE_1_PSI = 0.0f;
float AVG_PRESSURE_1_PSI = 0.0f;

// Válvula 2 (Pin 32)
float CURRENT_PRESSURE_2_RAW = 0.0f;
float CURRENT_PRESSURE_2_VOLTAGE = 0.0f;
float CURRENT_PRESSURE_2_PSI = 0.0f;
float AVG_PRESSURE_2_PSI = 0.0f;

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
String productId = "67d262aacf18fdaf14ec2e75";
String CONNECTION_TYPE = "Sin Red";
const char* controllerId = "68cb5159a742c4cf5c4b53b1";

// URLs para HTTPClient (WiFi)
const char* loginUrl = "http://164.92.95.176:3009/api/v1.0/auth/login";
const char* controllerUrl = "http://164.92.95.176:3009/api/v1.0/controllers/68cb5159a742c4cf5c4b53b1";
const char* dataUrl = "http://164.92.95.176:3009/api/v1.0/data";

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
  char buffer[256];
  va_list args;
  va_start(args, formato);
  vsnprintf(buffer, sizeof(buffer), formato, args);
  va_end(args);

  Serial.printf("[%s] %s\n", titulo, buffer);
  SerialBT.printf("[%s] %s\n", titulo, buffer);
}

void logWithTimestamp(const char* titulo, const char* formato, ...) {
  char buffer[256];
  va_list args;
  va_start(args, formato);
  vsnprintf(buffer, sizeof(buffer), formato, args);
  va_end(args);

  unsigned long timestamp = millis();
  Serial.printf("[%s][%lu] %s\n", titulo, timestamp, buffer);
  SerialBT.printf("[%s][%lu] %s\n", titulo, timestamp, buffer);
}

// ================== SENSOR FUNCTIONS ==================
// Variables para presión
float pressure1Filtered = 0.0f;
float pressure2Filtered = 0.0f;

float readAvgRaw(int pin, int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delay(2);
  }
  return (float)sum / samples;
}

void readPressureSensors() {
  static bool firstRead = true;
  
  // Leer Válvula 1 (Pin 35)
  float raw1 = readAvgRaw(PRESSURE_PIN_1, N_SAMPLES);
  float voltage1 = raw1 * (3.3 / 4095.0);
  float psi1 = raw1 * RAW_TO_PSI;

  // Leer Válvula 2 (Pin 32)
  float raw2 = readAvgRaw(PRESSURE_PIN_2, N_SAMPLES);
  float voltage2 = raw2 * (3.3 / 4095.0);
  float psi2 = raw2 * RAW_TO_PSI;

  // Aplicar filtro exponencial
  if (firstRead) {
    pressure1Filtered = psi1;
    pressure2Filtered = psi2;
    firstRead = false;
  } else {
    pressure1Filtered = pressure1Filtered * (1 - ALPHA) + psi1 * ALPHA;
    pressure2Filtered = pressure2Filtered * (1 - ALPHA) + psi2 * ALPHA;
  }
  
  CURRENT_PRESSURE_1_RAW = raw1;
  CURRENT_PRESSURE_1_VOLTAGE = voltage1;
  CURRENT_PRESSURE_1_PSI = pressure1Filtered;
  
  CURRENT_PRESSURE_2_RAW = raw2;
  CURRENT_PRESSURE_2_VOLTAGE = voltage2;
  CURRENT_PRESSURE_2_PSI = pressure2Filtered;
  
  // Calcular diferencia de presión
  PRESSURE_DIFFERENCE = abs(CURRENT_PRESSURE_1_PSI - CURRENT_PRESSURE_2_PSI);
  
  // ===== LOG DETALLADO PARA DEPURACIÓN =====
  Serial.printf("[PRESSURE][%lu] V1 RAW: %.0f -> %.3fV -> %.2f psi (filtered: %.2f) | V2 RAW: %.0f -> %.3fV -> %.2f psi (filtered: %.2f) | Diff: %.2f psi\n",
                millis(),
                raw1, voltage1, psi1, pressure1Filtered,
                raw2, voltage2, psi2, pressure2Filtered,
                PRESSURE_DIFFERENCE);
}

// ================== RELAY CONTROL ==================
void updateRelayControl() {
  bool previousRelayState = RELAY_STATE;
  
  // Activar relay si la diferencia de presión es >= 15 psi
  if (PRESSURE_DIFFERENCE >= PRESSURE_DIFF_THRESHOLD) {
    if (!RELAY_STATE) {
      RELAY_STATE = true;
      digitalWrite(RELAY_PIN, LOW);  // Activar relay
      // logWithTimestamp("RELAY", "🔄 CAMBIO: Relay ACTIVADO - Diferencia: %.2f psi (>= %.0f psi)", 
      //                  PRESSURE_DIFFERENCE, PRESSURE_DIFF_THRESHOLD);
    }
  } else {
    if (RELAY_STATE) {
      RELAY_STATE = false;
      digitalWrite(RELAY_PIN, HIGH);  // Desactivar relay
      // logWithTimestamp("RELAY", "🔄 CAMBIO: Relay DESACTIVADO - Diferencia: %.2f psi (< %.0f psi)", 
      //                  PRESSURE_DIFFERENCE, PRESSURE_DIFF_THRESHOLD);
    }
  }
  
  // if (RELAY_STATE != previousRelayState) {
  //   logWithTimestamp("RELAY", "Estado final: %s (Pin 16: %s) | Diff: %.2f psi", 
  //          RELAY_STATE ? "ACTIVADO" : "DESACTIVADO", 
  //          digitalRead(RELAY_PIN) ? "HIGH" : "LOW",
  //          PRESSURE_DIFFERENCE);
  // }
}

// ================== SYSTEM LOGIC ==================
void updateSystemLogic() {
  // Actualizar control del relay basado en diferencia de presión
  updateRelayControl();
  
  // Actualizar estado de conexión real
  updateNetworkStatus();
}

// ================== NETWORK FUNCTIONS ==================
void initializeNetwork() {
  // logger("NET", "🌐 INICIANDO CONECTIVIDAD DE RED...");
  // logWithTimestamp("NET", "=== INICIO DE INICIALIZACIÓN DE RED ===");
  
  // Mostrar tabla inmediatamente
  drawCompleteDisplay();
  
  // Mostrar mensaje de estado inicial
  drawStatusMessage("Iniciando AquaTech Controller...", TFT_BLUE);
  delay(1000);
  
  // Reset del chip Ethernet
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(200);
  digitalWrite(PIN_RST, HIGH);
  delay(200);
  pinMode(PIN_INT, INPUT);
  
  // Inicializar SPI y Ethernet
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);
  
  // Mostrar estado de conexión por cable
  drawStatusMessage("Intentando conectar usando cable...", TFT_BLUE);
  updateNetworkIconDuringConnection("Cable");
  
  // Intentar Ethernet primero
  // logger("NET", "Intentando conexión Ethernet...");
  drawStatusMessage("Intentando conectar usando cable...", TFT_BLUE);
  
  connectEthernet();
  
  if (connectedEthernet) {
    // logger("NET", "✅ Ethernet conectado exitosamente");
    drawStatusMessage("Cable OK - IP: " + deviceIP+ "      ", TFT_GREEN);
    delay(2000);
  } else {
    // logger("NET", "❌ Ethernet falló - continuando con WiFi");
  }
  
  if (!connectedEthernet) {
    // logger("NET", "Ethernet no disponible, intentando WiFi guardado (3s timeout)...");
    drawStatusMessage("Intentando WiFi...", TFT_ORANGE);
    updateNetworkIconDuringConnection("WiFi");
    
    if (!tryConnectSavedWiFi(3000)) {
      // logger("NET", "Sin credenciales WiFi - esperando Bluetooth");
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
  // logWithTimestamp("NET", "=== INICIALIZACIÓN DE RED COMPLETADA ===");
  // logger("NET", "Estado final - IP: %s, Ethernet: %s, WiFi: %s", 
  //        deviceIP.c_str(), 
  //        connectedEthernet ? "true" : "false",
  //        connectedWiFi ? "true" : "false");
  
  // Limpiar mensaje de estado después de 3 segundos
  delay(3000);
  clearStatusMessage();
}

// ================== API FUNCTIONS ==================
bool testServerConnectivity() {
  if (!connectedEthernet && !connectedWiFi) {
    return false;
  }
  
  logger("API", "Probando conectividad al servidor...");
  
  if (connectedWiFi) {
    logger("API", "Test via WiFi con HTTPClient");
    HTTPClient http;
    http.begin("http://164.92.95.176:3009/");
    http.setTimeout(3000);
    
    int httpCode = http.GET();
    http.end();
    
    if (httpCode > 0) {
      logger("API", "Test de conectividad via WiFi: OK (código: %d)", httpCode);
      return true;
    } else {
      logger("API", "Test de conectividad via WiFi: FALLO (código: %d)", httpCode);
      return false;
    }
    
  } else if (connectedEthernet) {
    logger("API", "Test via Ethernet con EthernetClient");
    EthernetClient testClient;
    testClient.setTimeout(2000);
    
    if (testClient.connect(serverIp, serverPort)) {
      testClient.stop();
      logger("API", "Test de conectividad via Ethernet: OK");
      return true;
    } else {
      logger("API", "Test de conectividad via Ethernet: FALLO");
      return false;
    }
  }
  
  return false;
}

void obtenerToken() {
  if (!connectedEthernet && !connectedWiFi) {
    logger("API", "Sin conexión de red para obtener token");
    return;
  }
  
  logger("API", "Obteniendo token de autenticación...");
  
  String loginPayload = "{\"email\":\"esp32@lcc.com.mx\",\"password\":\"Esp32*\"}";
  String response = "";
  
  if (connectedWiFi) {
    logger("API", "Usando HTTPClient (WiFi) para login");
    HTTPClient http;
    http.begin(loginUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);
    
    int httpCode = http.POST(loginPayload);
    
    if (httpCode > 0) {
      response = http.getString();
      logger("API", "Login exitoso via WiFi, código: %d", httpCode);
    } else {
      logger("API", "Error en login via WiFi, código: %d", httpCode);
      http.end();
      return;
    }
    http.end();
    
  } else if (connectedEthernet) {
    logger("API", "Usando EthernetClient para login");
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      logger("API", "Error: No se pudo conectar al servidor via Ethernet");
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
        logger("API", "Token obtenido exitosamente");
        return;
      } else if (doc.containsKey("access_token")) {
        jwtToken = doc["access_token"].as<String>();
        logger("API", "Access Token obtenido exitosamente");
        return;
      }
    }

    int tokenIndex = response.indexOf("\"token\":\"");
    if (tokenIndex != -1) {
      int start = tokenIndex + 9;
      int end = response.indexOf("\"", start);
      jwtToken = response.substring(start, end);
      logger("API", "Token obtenido con fallback");
    } else {
      logger("API", "Error: No se encontró token en la respuesta");
    }
  } else {
    logger("API", "Error: Respuesta vacía del servidor");
  }
}

String getControllerById() {
  if (!connectedEthernet && !connectedWiFi) {
    logger("API", "Sin conexión de red para obtener datos del controller");
    return "";
  }
  
  if (jwtToken == "") {
    logger("API", "Sin token - obteniendo token primero...");
    obtenerToken();
    delay(500);
    if (jwtToken == "") {
      logger("API", "Error: No se pudo obtener token para GET controller");
      return "";
    }
  }

  logger("API", "Obteniendo datos del controller...");
  
  String response = "";
  String json = "";
  
  if (connectedWiFi) {
    logger("API", "Usando HTTPClient (WiFi) para controller");
    HTTPClient http;
    http.begin(controllerUrl);
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);
    
    int httpCode = http.GET();
    
    if (httpCode > 0) {
      response = http.getString();
      json = response;
      logger("API", "Controller data obtenido via WiFi, código: %d", httpCode);
    } else {
      logger("API", "Error obteniendo controller via WiFi, código: %d", httpCode);
      http.end();
      return "";
    }
    http.end();
    
  } else if (connectedEthernet) {
    logger("API", "Usando EthernetClient para controller");
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      logger("API", "Error: No se pudo conectar al servidor via Ethernet");
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
      logger("API", "Error: Respuesta sin JSON válido");
      return "";
    }
    json = response.substring(jsonStart);
  }

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    logger("API", "Error: No se pudo parsear JSON del controller");
    return json;
  }

  logger("API", "=== DATOS RECIBIDOS DEL CONTROLLER ===");
  
  if (doc.containsKey("update_controller_time")) {
    logger("API", "Servidor - update_controller_time: %lu ms", doc["update_controller_time"].as<unsigned long>());
  }
  if (doc.containsKey("loop_time")) {
    logger("API", "Servidor - loop_time: %lu ms", doc["loop_time"].as<unsigned long>());
  }
  if (doc.containsKey("productId")) {
    logger("API", "Servidor - productId: %s", doc["productId"].as<String>().c_str());
  }
  if (doc.containsKey("product_name")) {
    logger("API", "Servidor - product_name: %s", doc["product_name"].as<String>().c_str());
  }
  if (doc.containsKey("reset_pending")) {
    logger("API", "Servidor - reset_pending: %s", doc["reset_pending"].as<bool>() ? "true" : "false");
  }
  if (doc.containsKey("flush_time")) {
    logger("API", "Servidor - flush_time: %lu ms", doc["flush_time"].as<unsigned long>());
  }
  
  logger("API", "=== VALORES ACTUALES (ANTES) ===");
  logger("API", "Local - update_controller_time: %lu ms", updateControllerTime);
  logger("API", "Local - loop_time: %lu ms", loopTime);
  logger("API", "Local - productId: %s", productId.c_str());
  logger("API", "Local - product_name: %s", product_name.c_str());
  logger("API", "Local - reset_pending: %s", reset_pending ? "true" : "false");
  logger("API", "Local - flush_time: %lu ms", flush_time);
  
  bool updated = false;
  logger("API", "=== APLICANDO CAMBIOS ===");
  
  if (doc.containsKey("update_controller_time")) {
    unsigned long newUpdateTime = doc["update_controller_time"].as<unsigned long>();
    if (newUpdateTime >= 10000 && newUpdateTime != updateControllerTime) {
      unsigned long oldValue = updateControllerTime;
      updateControllerTime = newUpdateTime;
      logger("API", "✓ CAMBIADO update_controller_time: %lu → %lu ms", oldValue, updateControllerTime);
      updated = true;
    } else {
      logger("API", "- update_controller_time sin cambios (%lu ms)", updateControllerTime);
    }
  } else {
    logger("API", "- update_controller_time no presente en respuesta");
  }

  if (doc.containsKey("loop_time")) {
    unsigned long newLoopTime = doc["loop_time"].as<unsigned long>();
    if (newLoopTime >= 500 && newLoopTime != loopTime) {
      unsigned long oldValue = loopTime;
      loopTime = newLoopTime;
      logger("API", "✓ CAMBIADO loop_time: %lu → %lu ms", oldValue, loopTime);
      updated = true;
    } else {
      logger("API", "- loop_time sin cambios (%lu ms)", loopTime);
    }
  } else {
    logger("API", "- loop_time no presente en respuesta");
  }

  if (doc.containsKey("productId")) {
    String newProductId = doc["productId"].as<String>();
    if (newProductId.length() > 0 && newProductId != productId) {
      String oldValue = productId;
      productId = newProductId;
      logger("API", "✓ CAMBIADO productId: %s → %s", oldValue.c_str(), productId.c_str());
      updated = true;
    } else {
      logger("API", "- productId sin cambios (%s)", productId.c_str());
    }
  } else {
    logger("API", "- productId no presente en respuesta");
  }

  if (doc.containsKey("product_name")) {
    String newProductName = doc["product_name"].as<String>();
    if (newProductName != product_name) {
      String oldValue = product_name;
      product_name = newProductName;
      logger("API", "✓ CAMBIADO product_name: '%s' → '%s'", oldValue.c_str(), product_name.c_str());
      updated = true;
    } else {
      logger("API", "- product_name sin cambios ('%s')", product_name.c_str());
    }
  } else {
    logger("API", "- product_name no presente en respuesta");
  }

  if (doc.containsKey("reset_pending")) {
    bool newResetPending = doc["reset_pending"].as<bool>();
    if (newResetPending != reset_pending) {
      bool oldValue = reset_pending;
      reset_pending = newResetPending;
      logger("API", "✓ CAMBIADO reset_pending: %s → %s", 
             oldValue ? "true" : "false", 
             reset_pending ? "true" : "false");
      updated = true;
      
      if (reset_pending) {
        logger("API", "⚠️  REINICIO PENDIENTE - Se ejecutará en el próximo ciclo");
      }
    } else {
      logger("API", "- reset_pending sin cambios (%s)", reset_pending ? "true" : "false");
    }
  } else {
    logger("API", "- reset_pending no presente en respuesta");
  }

  if (doc.containsKey("flush_time")) {
    unsigned long newFlushTime = doc["flush_time"].as<unsigned long>();
    if (newFlushTime != flush_time) {
      unsigned long oldValue = flush_time;
      flush_time = newFlushTime;
      logger("API", "✓ CAMBIADO flush_time: %lu → %lu ms", oldValue, flush_time);
      updated = true;
    } else {
      logger("API", "- flush_time sin cambios (%lu ms)", flush_time);
    }
  } else {
    logger("API", "- flush_time no presente en respuesta");
  }

  if (response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
    logger("API", "Token expirado - será renovado en próxima llamada");
    jwtToken = "";
  }

  logger("API", "=== RESUMEN ===");
  if (updated) {
    logger("API", "✓ CONFIGURACIONES ACTUALIZADAS desde servidor");
  } else {
    logger("API", "- Sin cambios necesarios (valores ya sincronizados)");
  }
  logger("API", "=====================================");

  return json;
}

void enviarDatos(const char* productIdParam, float pressure1PSI, float pressure2PSI, float pressureDiff, float temperature) {
  if (!connectedEthernet && !connectedWiFi) {
    logger("API", "Sin conexión de red para enviar datos");
    return;
  }
  
  if (jwtToken == "") {
    logger("API", "Sin token - obteniendo token primero...");
    obtenerToken();
    delay(500);
    if (jwtToken == "") {
      logger("API", "Error: No se pudo obtener token para enviar datos");
      return;
    }
  }

  logWithTimestamp("API", "=== INICIANDO ENVÍO DE DATOS ===");
  logWithTimestamp("API", "Datos - V1: %.2f psi, V2: %.2f psi, Diff: %.2f psi, Relay: %s, Temp: %.2f°C", 
                   pressure1PSI, pressure2PSI, pressureDiff, RELAY_STATE ? "ON" : "OFF", temperature);

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

  logWithTimestamp("API", "Payload JSON: %s", jsonPayload.c_str());

  if (connectedWiFi) {
    logWithTimestamp("API", "Enviando datos via WiFi con HTTPClient");
    HTTPClient http;
    http.begin(dataUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);
    
    int httpCode = http.POST(jsonPayload);
    
    if (httpCode > 0) {
      String response = http.getString();
      logWithTimestamp("API", "✅ Datos enviados exitosamente via WiFi, código: %d", httpCode);
      logWithTimestamp("API", "Respuesta: %s", response.c_str());
    } else {
      logWithTimestamp("API", "❌ Error enviando datos via WiFi, código: %d", httpCode);
    }
    http.end();
    
  } else if (connectedEthernet) {
    logWithTimestamp("API", "Enviando datos via Ethernet con EthernetClient");
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      logWithTimestamp("API", "❌ Error: No se pudo conectar al servidor via Ethernet");
      return;
    }

    String request = "";
    request += "POST /api/v1.0/data HTTP/1.1\r\n";
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

    if (response.length() > 0) {
      logWithTimestamp("API", "✅ Datos enviados exitosamente via Ethernet");
      logWithTimestamp("API", "Respuesta: %s", response.c_str());
    } else {
      logWithTimestamp("API", "❌ Error: Sin respuesta del servidor via Ethernet");
    }
  }

  if (jwtToken.length() > 0) {
    static unsigned long lastTokenCheck = 0;
    if (millis() - lastTokenCheck > 300000) {
      logger("API", "Renovando token preventivamente...");
      jwtToken = "";
      lastTokenCheck = millis();
    }
  }

  logWithTimestamp("API", "=== ENVÍO DE DATOS COMPLETADO ===");
}

bool patchControllerResetPending() {
  if (!connectedEthernet && !connectedWiFi) {
    logger("API", "Sin conexión de red para actualizar reset_pending");
    return false;
  }
  
  if (jwtToken == "") {
    logger("API", "Sin token - obteniendo token primero...");
    obtenerToken();
    delay(500);
    if (jwtToken == "") {
      logger("API", "Error: No se pudo obtener token para PATCH controller");
      return false;
    }
  }

  logger("API", "Actualizando reset_pending a false...");
  
  String patchPayload = "{\"reset_pending\":false}";
  String response = "";
  
  if (connectedWiFi) {
    logger("API", "Usando HTTPClient (WiFi) para PATCH controller");
    HTTPClient http;
    http.begin(controllerUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);
    
    int httpCode = http.PATCH(patchPayload);
    
    if (httpCode > 0) {
      response = http.getString();
      logger("API", "PATCH controller exitoso via WiFi, código: %d", httpCode);
    } else {
      logger("API", "Error en PATCH controller via WiFi, código: %d", httpCode);
      http.end();
      return false;
    }
    http.end();
    
  } else if (connectedEthernet) {
    logger("API", "Usando EthernetClient para PATCH controller");
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      logger("API", "Error: No se pudo conectar al servidor via Ethernet");
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

  if (response.length() > 0) {
    logger("API", "✅ reset_pending actualizado exitosamente");
    logger("API", "Respuesta: %s", response.c_str());
    return true;
  } else {
    logger("API", "❌ Error: Sin respuesta del servidor");
    return false;
  }
}


void connectEthernet() {
  logger("NET", "Intentando conexión Ethernet...");
  
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(200);
  digitalWrite(PIN_RST, HIGH);
  delay(200);
  pinMode(PIN_INT, INPUT);
  
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);
  
  logger("NET", "Inicializando Ethernet.begin()...");
  unsigned long startTime = millis();
  
  int result = Ethernet.begin(mac);
  
  unsigned long elapsed = millis() - startTime;
  logger("NET", "Ethernet.begin() completado en %lu ms, resultado: %d", elapsed, result);
  
  if (result == 0) {
    logger("NET", "❌ Ethernet.begin() falló - result = 0");
    connectedEthernet = false;
    deviceIP = "0.0.0.0";
    return;
  }
  
  IPAddress ip = Ethernet.localIP();
  if (ip == INADDR_NONE) {
    logger("NET", "❌ IP inválida después de Ethernet.begin()");
    connectedEthernet = false;
    deviceIP = "0.0.0.0";
    return;
  }
  
  deviceIP = ip.toString();
  connectedEthernet = true;
  CONNECTION_TYPE = "Cable";
  CURRENT_CONNECTION_STATE = true;
  logger("NET", "✅ Ethernet conectado - IP: %s", deviceIP.c_str());
}

bool tryConnectSavedWiFi(unsigned long timeoutMs = 5000) {
  logger("NET", "Intentando WiFi con credenciales guardadas...");
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
    logger("NET", "WiFi conectado - IP: %s", deviceIP.c_str());
    return true;
  }
  
  connectedWiFi = false;
  logger("NET", "WiFi timeout después de %lu ms", timeoutMs);
  return false;
}

void connectWiFi(const char* ssid, const char* password, unsigned long timeoutMs = 5000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  logger("NET", "Conectando a %s...", ssid);
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    deviceIP = WiFi.localIP().toString();
    connectedWiFi = true;
    CONNECTION_TYPE = "WiFi";
    CURRENT_CONNECTION_STATE = true;
    logger("NET", "WiFi conectado - IP: %s", deviceIP.c_str());
  } else {
    logger("NET", "No se pudo conectar a WiFi");
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
        logger("BT", "Borrando credenciales WiFi...");
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
        
        logger("BT", "Recibido SSID: %s", ssid.c_str());
        
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
          
          logger("BT", "WiFi conectado! IP: %s", deviceIP.c_str());
          delay(300);
          ESP.restart();
        } else {
          logger("BT", "No se pudo conectar con esas credenciales");
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
  int iconWidth = 80;
  int totalIconWidth = iconWidth * 4;
  int spacing = (screenWidth - totalIconWidth) / 5;
  
  if (spacing < 5) {
    spacing = 5;
    iconWidth = (screenWidth - spacing * 5) / 4;
  }
  
  int iconY = 10;
  int pumpX = spacing + (iconWidth + spacing) * 2;  // Tercer icono
  
  clearArea(pumpX, iconY, iconWidth, 80);
  
  drawPumpIcon(pumpX, iconY, RELAY_STATE);
  
  // logWithTimestamp("PUMP", "Icono actualizado: %s (Diff: %.2f psi)", 
  //                  RELAY_STATE ? "ACTIVA" : "INACTIVA", PRESSURE_DIFFERENCE);
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
  
  // Dibujar manómetro
  tft.drawCircle(x + radius, y + radius, 18, TFT_WHITE);
  tft.drawCircle(x + radius, y + radius, 17, TFT_WHITE);
  
  // Aguja del manómetro
  int needleX = x + radius + 10 * cos(0.785); // 45 grados
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
  
  // Animación de bomba
  if (state && millis() - lastPumpUpdate > 500) {
    currentPumpSpeed = 0.3;
    pumpRotation += currentPumpSpeed;
    if (pumpRotation > 6.28) pumpRotation = 0;
    lastPumpUpdate = millis();
  } else if (!state) {
    pumpRotation = 0;
  }
  
  // Dibujar aspas
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
  tft.setCursor(x + 8, y + 60);
  tft.print("Bomba");
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
  
  // logWithTimestamp("STATUS", "Mensaje: %s", message.c_str());
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
  int iconWidth = 80;
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
    drawBluetoothIcon(networkX + 25, iconY + 15);
  }
  
  // logWithTimestamp("NET", "Icono actualizado: %s", connectionType.c_str());
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
  // logWithTimestamp("DISPLAY", "🔄 Redibujando pantalla completa...");
  
  tft.fillScreen(TFT_WHITE);
  
  int screenWidth = tft.width();
  int iconWidth = 60;
  int totalIconWidth = iconWidth * 4;
  int spacing = (screenWidth - totalIconWidth) / 5;
  
  if (spacing < 5) {
    spacing = 5;
    iconWidth = (screenWidth - spacing * 5) / 4;
  }
  
  int iconY = 10;
  int currentX = spacing;
  
  drawPowerIcon(currentX, iconY, POWER_ON);
  currentX += iconWidth + spacing;
  
  drawPressureIcon(currentX, iconY);
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
  tft.print("Valvula 1:");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.setCursor(avgX, startY + 8);
  tft.printf("/ %.0f", AVG_PRESSURE_1_PSI);
  
  tft.setTextColor(0xF800, TFT_WHITE);
  tft.setCursor(10, startY + lineH);
  tft.print("Valvula 2:");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.setCursor(avgX, startY + lineH + 8);
  tft.printf("/ %.0f", AVG_PRESSURE_2_PSI);
  
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
  
  // logWithTimestamp("DISPLAY", "✅ Pantalla completa redibujada");
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
  tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
  tft.setCursor(valueX, startY);
  tft.printf("%.2f psi", CURRENT_PRESSURE_1_PSI);
  lastPressure1PSI = CURRENT_PRESSURE_1_PSI;
  
  // Actualizar valor de Válvula 2
  clearArea(valueX, startY + lineH, avgX - valueX - 10, 30);
  tft.setTextSize(2);
  tft.setTextColor(0xF800, TFT_WHITE);
  tft.setCursor(valueX, startY + lineH);
  tft.printf("%.2f psi", CURRENT_PRESSURE_2_PSI);
  lastPressure2PSI = CURRENT_PRESSURE_2_PSI;
  
  // Actualizar diferencia de presión
  clearArea(valueX, startY + lineH * 2, avgX - valueX - 10, 30);
  tft.setTextSize(2);
  uint16_t diffColor = (PRESSURE_DIFFERENCE >= PRESSURE_DIFF_THRESHOLD) ? TFT_RED : TFT_BLUE;
  tft.setTextColor(diffColor, TFT_WHITE);
  tft.setCursor(valueX, startY + lineH * 2);
  tft.printf("%.2f psi", PRESSURE_DIFFERENCE);
  
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
    int spacing = (screenWidth - iconWidth * 4) / 5;
    if (spacing < 5) {
      spacing = 5;
      iconWidth = (screenWidth - spacing * 5) / 4;
    }
    int pumpX = spacing + (iconWidth + spacing) * 2;
    
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
      // logWithTimestamp("TIMER", "Contador actualizado: %lu seg (desde último updateController)", displayTime);
    }
    
    lastTimerUpdate = now;
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  SerialBT.begin("AquaTech_Pressure");
  // logger("INFO", "=== INICIANDO AQUATECH PRESSURE MONITOR ===");
  Serial.println("\n\n=== MODO DEPURACIÓN PRESIÓN ACTIVADO ===");
  Serial.println("Solo se mostrarán lecturas de presión\n");
  
  // Configurar pines
  pinMode(PRESSURE_PIN_1, INPUT);  // Válvula 1 (Pin 35)
  pinMode(PRESSURE_PIN_2, INPUT);  // Válvula 2 (Pin 32)
  pinMode(RELAY_PIN, OUTPUT);      // Relay (Pin 16)
  
  // Configurar ADC para sensores de presión
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(PRESSURE_PIN_1, ADC_11db);
  analogSetPinAttenuation(PRESSURE_PIN_2, ADC_11db);
  
  // Estado inicial del relay (desactivado)
  digitalWrite(RELAY_PIN, HIGH);
  RELAY_STATE = false;
  // logger("RELAY", "Estado inicial: DESACTIVADO (Pin 16: HIGH)");
  
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
  tft.writedata(0xE8);  // Cambiado de 0x28 a 0xE8 para rotar 180°
  
  tft.fillScreen(TFT_WHITE);
  
  // Inicializar conectividad de red
  initializeNetwork();
  
  // Obtener configuración del controller si hay conexión
  // if (connectedEthernet || connectedWiFi) {
  //   logger("SETUP", "Obteniendo configuración inicial del controller...");
  //   getControllerById();
  // } else {
  //   logger("SETUP", "Sin conexión - usando valores por defecto");
  // }
  
  // logger("INFO", "=== SETUP COMPLETADO - PRESSURE MONITOR ===");
  Serial.println("=== INICIANDO MONITOREO DE PRESIÓN ===\n");
}

// ================== MAIN LOOP ==================
void loop() {
  unsigned long now = millis();
  
  // ====== LECTURA CONTINUA DE SENSORES ======
  readPressureSensors();
  updateSystemLogic();
  
  // ====== ACTUALIZAR ICONO DE LA BOMBA EN TIEMPO REAL ======
  static bool lastRelayStateLoop = false;
  if (RELAY_STATE != lastRelayStateLoop) {
    updatePumpIcon();
    lastRelayStateLoop = RELAY_STATE;
  }
  
  // ====== FLUJO 1: CONTADOR DE TIEMPO INDEPENDIENTE ======
  updateDisplayTimer();
  
  // ====== FLUJO 2: ACTUALIZACIÓN DE PANTALLA Y ENVÍO DE DATOS ======
  if (now - lastLoop >= loopTime) {
    // logWithTimestamp("LOOP", "🔄 ACTUALIZANDO VALORES (loopTime: %lu ms)", loopTime);
    updateDisplayValues();
    
    // Enviar datos si hay conexión (COMENTADO PARA DEPURACIÓN)
    // if (connectedEthernet || connectedWiFi) {
    //   enviarDatos(productId.c_str(), CURRENT_PRESSURE_1_PSI, CURRENT_PRESSURE_2_PSI, PRESSURE_DIFFERENCE, 25.0);
    // }
    
    lastLoop = now;
    // logWithTimestamp("LOOP", "✅ ACTUALIZACIÓN DE VALORES COMPLETADA");
  }
  
  // ====== ACTUALIZACIÓN ADICIONAL DE VALORES CADA 500ms ======
  static unsigned long lastQuickUpdate = 0;
  if (now - lastQuickUpdate >= 500) {
    updateDisplayValues();
    lastQuickUpdate = now;
  }
  
  // ====== FLUJO 4: CICLO COMPLETO updateControllerTime ======
  if (now - lastUpdateController >= updateControllerTime) {
    // logWithTimestamp("CYCLE", "🔄 INICIANDO CICLO COMPLETO updateControllerTime (%lu ms)", updateControllerTime);
    
    // Actualizar configuración del controller si hay conexión
    // if (connectedEthernet || connectedWiFi) {
    //   logWithTimestamp("API", "Obteniendo configuración actualizada del controller...");
    //   getControllerById();
    //   
    //   // Verificar si se solicita reinicio remoto
    //   if (reset_pending) {
    //     logWithTimestamp("RESET", "⚠️ REINICIO REMOTO DETECTADO - Ejecutando...");
    //     patchControllerResetPending();
    //     delay(1000);
    //     ESP.restart();
    //   }
    // }
    
    // Redibujar display completo
    // logWithTimestamp("DISPLAY", "Redibujando pantalla completa...");
    drawCompleteDisplay();
    lastUpdateController = now;
    
    // if (connectedEthernet || connectedWiFi) {
    //   logWithTimestamp("CYCLE", "✅ CICLO COMPLETO FINALIZADO - Con red");
    // } else {
    //   logWithTimestamp("CYCLE", "✅ CICLO COMPLETO FINALIZADO - Sin red");
    // }
  }
  
  // ====== MANEJO DE BLUETOOTH (solo si no hay red) ======
  if (!connectedEthernet && !connectedWiFi) {
    handleBluetoothWiFi();
  }
  
  // Delay mínimo
  delay(10);
}

