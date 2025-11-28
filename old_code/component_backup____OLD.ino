// ================== AQUATECH CONTROLLER ==================
#include <SPI.h>
#include <Ethernet.h>
#include <TFT_eSPI.h>
#include "BluetoothSerial.h"
#include <driver/adc.h>
#include <time.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ================== BLUETOOTH SERIAL ==================
BluetoothSerial SerialBT;

// ================== CONFIGURATION ==================
// Timing
unsigned long updateControllerTime = 10000; // 10 seconds
unsigned long loopTime = 1000;             // 1 second
unsigned long lastUpdateController = 0;
unsigned long lastLoop = 0;
unsigned long lastPumpUpdate = 0;

// Network
bool connectedEthernet = false;
bool connectedWiFi = false;
String deviceIP = "0.0.0.0";
String jwtToken = "";
String productId = "67d262aacf18fdaf14ec2e75"; // Removido const para permitir actualización remota
String product_name = ""; // Nombre del producto desde servidor
const char* controllerId = "68cb5159a742c4cf5c4b53b1";

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

// URLs para HTTPClient (WiFi)
const char* loginUrl = "http://164.92.95.176:3009/api/v1.0/auth/login";
const char* controllerUrl = "http://164.92.95.176:3009/api/v1.0/controllers/68cb5159a742c4cf5c4b53b1";
const char* controllerPatchUrl = "http://164.92.95.176:3009/api/v1.0/controllers/68cb5159a742c4cf5c4b53b1";


EthernetClient client;

// Pin definitions
const int TDS_PIN = 32;
const int FLOW_PIN_PROD = 15;
const int FLOW_PIN_RECH = 4;
const int RELAY_PIN = 19;
const int CONTROL_PIN_1 = 27;
const int CONTROL_PIN_2 = 25;

// Sensor configuration
float CALIBRATION_FACTOR_TDS = 1.58f; // Removido const para permitir actualización remota
const float ALPHA = 0.1f; // Exponential filter
float FLOW_FACTOR = 55.0f; // Removido const para permitir actualización remota

// Control variables
bool reset_pending = false; // Para reinicio remoto del controlador
unsigned long flush_time = 0; // Para futuras funcionalidades de flush

// System state
float CURRENT_TDS = 75.0f;  // Inicializar con valor razonable
float CURRENT_FLOW_PROD = 2.5f;  // Valor inicial visible
float CURRENT_FLOW_RECH = 0.8f;  // Valor inicial visible
float CURRENT_VOLUME_PROD = 0.0f;
float CURRENT_VOLUME_RECH = 0.0f;
float AVG_FLOW_PROD = 150.0f;
float AVG_FLOW_RECH = 130.0f;
float AVG_TDS = 75.0f;

bool CURRENT_CONNECTION_STATE = false;
bool RELAY_STATE = true;
bool TANK_FULL = false;
bool POWER_ON = true;
String CONNECTION_TYPE = "Cable";

// Flow sensors
volatile int FLOW_PULSE_PROD = 0;
volatile int FLOW_PULSE_RECH = 0;

// Pump animation
float pumpRotation = 0.0;
float basePumpSpeed = 0.15;
float currentPumpSpeed = 0.15;

// Display state tracking (to reduce flicker)
static bool lastPowerState = true;
static bool lastTankState = false;
static bool lastRelayState = true;
static bool lastConnectionState = false;
static String lastConnectionType = "";
static float lastTDS = -999;  // Forzar primera actualización
static float lastFlowProd = -999;  // Forzar primera actualización
static float lastFlowRech = -999;  // Forzar primera actualización
static unsigned long lastSyncTime = 0;
static String lastProductName = "___FORCE_UPDATE___";  // Forzar primera actualización
static bool displayInitialized = false;

// Display
TFT_eSPI tft = TFT_eSPI();

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

// ================== SENSOR FUNCTIONS ==================
// Variables para TDS mejorado (del código probado)
const int NUM_SAMPLES = 50;   // Número de muestras para promediar
float tdsFiltered = 0.0f;     // Variable para filtro exponencial
float lastVoltage = 0.0f;     // Último voltaje leído

// Función para leer y promediar N muestras del ADC
float readAverageVoltage() {
  long sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    sum += analogRead(TDS_PIN);
    delay(2);
  }
  float adcAverage = sum / (float)NUM_SAMPLES;
  return adcAverage * 3.3 / 4095.0; // Convertir a voltaje
}

void readTDSSensor() {
  static bool firstRead = true;
  
  float voltage = readAverageVoltage();
  lastVoltage = voltage;

  // Umbral mínimo de voltaje para protección (del código probado)
  const float V_MIN = 0.05;   // Voltios, ajustable según tu sensor
  float tds_ppm;

  if (voltage < V_MIN) {
    // Voltaje muy bajo: establecer TDS mínimo o aplicar factor de compensación
    tds_ppm = 35.0 + (voltage / V_MIN) * (CALIBRATION_FACTOR_TDS * voltage * 100.0);
    // Esto asegura que nunca baje de ~35 ppm
  } else {
    tds_ppm = voltage * CALIBRATION_FACTOR_TDS * 100.0;
  }

  // Filtro exponencial (del código probado)
  tdsFiltered = tdsFiltered * (1 - ALPHA) + tds_ppm * ALPHA;
  
  // Forzar valor inicial para que aparezca en display
  if (firstRead) {
    CURRENT_TDS = tdsFiltered;
    firstRead = false;
  } else {
    CURRENT_TDS = tdsFiltered;
  }
}

// Variables para cálculo de flujo mejorado (del código probado)
float flowRateProd = 0.0;
float flowRateRech = 0.0;
float volumenProd = 0.0;
float volumenRech = 0.0;
unsigned long lastFlowCalc = 0;

void updateFlowRates() {
  unsigned long now = millis();
  unsigned long deltaTime = now - lastFlowCalc;  // tiempo transcurrido en ms

  if (deltaTime >= loopTime) {
    float deltaMinutes = deltaTime / 60000.0; // convertir ms a minutos

    // Usar el factor de calibración remoto FLOW_FACTOR (kFactor)
    flowRateProd = FLOW_PULSE_PROD / FLOW_FACTOR; // L/min
    flowRateRech = FLOW_PULSE_RECH / FLOW_FACTOR; // L/min

    volumenProd += flowRateProd * deltaMinutes; // L
    volumenRech += flowRateRech * deltaMinutes; // L

    // Actualizar variables globales para display
    CURRENT_FLOW_PROD = flowRateProd;
    CURRENT_FLOW_RECH = flowRateRech;
    CURRENT_VOLUME_PROD = volumenProd;
    CURRENT_VOLUME_RECH = volumenRech;

    FLOW_PULSE_PROD = 0;
    FLOW_PULSE_RECH = 0;
    lastFlowCalc = now;
  }
}

void IRAM_ATTR flowInterruptProd() { FLOW_PULSE_PROD++; }
void IRAM_ATTR flowInterruptRech() { FLOW_PULSE_RECH++; }

// ================== NETWORK FUNCTIONS ==================
void connectEthernet() {
  logger("NET", "Intentando conexión Ethernet...");
  
  if (Ethernet.begin(mac) == 0) {
    logger("NET", "DHCP Ethernet falló");
    connectedEthernet = false;
    deviceIP = "0.0.0.0";
  } else {
    IPAddress ip = Ethernet.localIP();
    deviceIP = ip.toString();
    connectedEthernet = true;
    CONNECTION_TYPE = "Cable";
    CURRENT_CONNECTION_STATE = true;
    logger("NET", "Ethernet conectado - IP: %s", deviceIP.c_str());
  }
}

bool tryConnectSavedWiFi(unsigned long timeoutMs = 8000) {
  logger("NET", "Intentando WiFi con credenciales guardadas...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // usar credenciales persistentes
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(300);
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
  return false;
}

void connectWiFi(const char* ssid, const char* password, unsigned long timeoutMs = 10000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  logger("NET", "Conectando a %s...", ssid);
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
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
      
      // Comando para borrar credenciales
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
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
          delay(500);
        }
        
        if (WiFi.status() == WL_CONNECTED) {
          deviceIP = WiFi.localIP().toString();
          connectedWiFi = true;
          CONNECTION_TYPE = "WiFi";
          CURRENT_CONNECTION_STATE = true;
          
          logger("BT", "WiFi conectado! IP: %s", deviceIP.c_str());
          delay(300);
          ESP.restart(); // reiniciar para aplicar configuración persistente
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
  // Actualizar estado de conexión
  if (connectedEthernet || connectedWiFi) {
    CURRENT_CONNECTION_STATE = true;
    deviceIP = connectedEthernet ? Ethernet.localIP().toString() : WiFi.localIP().toString();
  } else {
    CURRENT_CONNECTION_STATE = false;
    deviceIP = "0.0.0.0";
    CONNECTION_TYPE = "Cable"; // default
  }
}

// ================== API FUNCTIONS ==================
bool testServerConnectivity() {
  if (!connectedEthernet && !connectedWiFi) {
    return false;
  }
  
  
  logger("API", "Probando conectividad al servidor...");
  
  if (connectedWiFi) {
    // Test con HTTPClient para WiFi - más simple y robusto
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
    // Test con EthernetClient para Ethernet
    logger("API", "Test via Ethernet con EthernetClient");
    EthernetClient testClient;
    testClient.setTimeout(2000);
    
    if (testClient.connect(serverIp, serverPort)) {
      testClient.stop();
      logger("API", "Test de conectividad via Ethernet: OK");
      return true;
    } else {
      logger("API", "Test de conectividad via Ethernet: FALLO");
      logger("API", "IP Local: %s", deviceIP.c_str());
      logger("API", "Servidor destino: %s:%d", serverIp.toString().c_str(), serverPort);
      
      // Sugerencias para debugging
      static unsigned long lastSuggestion = 0;
      if (millis() - lastSuggestion > 300000) { // Cada 5 minutos
        logger("API", "SUGERENCIAS:");
        logger("API", "1. Verificar que servidor esté en línea");
        logger("API", "2. Probar: telnet %s %d", serverIp.toString().c_str(), serverPort);
        logger("API", "3. Revisar firewall/router");
        logger("API", "4. Verificar configuración de red local");
        lastSuggestion = millis();
      }
      
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
    // Usar HTTPClient para WiFi
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
    // Usar EthernetClient para Ethernet
    logger("API", "Usando EthernetClient para login");
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      logger("API", "Error: No se pudo conectar al servidor via Ethernet");
      return;
    }
    
    // Enviar petición HTTP cruda
    localClient.print("POST /api/v1.0/auth/login HTTP/1.1\r\n");
    localClient.print("Host: 164.92.95.176\r\n");
    localClient.print("Content-Type: application/json\r\n");
    localClient.print("Content-Length: ");
    localClient.println(loginPayload.length());
    localClient.print("Connection: close\r\n\r\n");
    localClient.print(loginPayload);

    // Leer respuesta
    unsigned long timeout = millis();
    while (millis() - timeout < 5000) {
      while (localClient.available()) {
        char c = localClient.read();
        response += c;
        timeout = millis();
      }
    }
    localClient.stop();
    
    // Separar headers del body para Ethernet
    int bodyIndex = response.indexOf("\r\n\r\n");
    if (bodyIndex != -1) {
      response = response.substring(bodyIndex + 4);
    }
  }

  // Parsear respuesta (común para ambos métodos)
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

    // Fallback con indexOf
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
    // Usar HTTPClient para WiFi
    logger("API", "Usando HTTPClient (WiFi) para controller");
    HTTPClient http;
    http.begin(controllerUrl);
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);
    
    int httpCode = http.GET();
    
    if (httpCode > 0) {
      response = http.getString();
      json = response; // HTTPClient ya devuelve solo el JSON
      logger("API", "Controller data obtenido via WiFi, código: %d", httpCode);
    } else {
      logger("API", "Error obteniendo controller via WiFi, código: %d", httpCode);
      http.end();
      return "";
    }
    http.end();
    
  } else if (connectedEthernet) {
    // Usar EthernetClient para Ethernet
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

  // Log de datos completos recibidos del servidor
  logger("API", "=== DATOS RECIBIDOS DEL CONTROLLER ===");
  
  // Mostrar todos los campos disponibles
  if (doc.containsKey("kfactor_flujo")) {
    logger("API", "Servidor - kfactor_flujo: %.2f", doc["kfactor_flujo"].as<float>());
  }
  if (doc.containsKey("kfactor_tds")) {
    logger("API", "Servidor - kfactor_tds: %.2f", doc["kfactor_tds"].as<float>());
  }
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
  
  // Mostrar valores actuales antes de actualizar
  logger("API", "=== VALORES ACTUALES (ANTES) ===");
  logger("API", "Local - kfactor_flujo: %.2f", FLOW_FACTOR);
  logger("API", "Local - kfactor_tds: %.2f", CALIBRATION_FACTOR_TDS);
  logger("API", "Local - update_controller_time: %lu ms", updateControllerTime);
  logger("API", "Local - loop_time: %lu ms", loopTime);
  logger("API", "Local - productId: %s", productId.c_str());
  logger("API", "Local - product_name: %s", product_name.c_str());
  logger("API", "Local - reset_pending: %s", reset_pending ? "true" : "false");
  logger("API", "Local - flush_time: %lu ms", flush_time);
  
  // Actualizar configuraciones si existen
  bool updated = false;
  logger("API", "=== APLICANDO CAMBIOS ===");
  
  if (doc.containsKey("kfactor_flujo")) {
    float newK = doc["kfactor_flujo"].as<float>();
    if (newK > 0 && newK != FLOW_FACTOR) {
      float oldValue = FLOW_FACTOR;
      FLOW_FACTOR = newK;
      logger("API", "✓ CAMBIADO kfactor_flujo: %.2f → %.2f", oldValue, FLOW_FACTOR);
      updated = true;
    } else {
      logger("API", "- kfactor_flujo sin cambios (%.2f)", FLOW_FACTOR);
    }
  } else {
    logger("API", "- kfactor_flujo no presente en respuesta");
  }
  
  if (doc.containsKey("kfactor_tds")) {
    float newTDS = doc["kfactor_tds"].as<float>();
    if (newTDS > 0 && newTDS != CALIBRATION_FACTOR_TDS) {
      float oldValue = CALIBRATION_FACTOR_TDS;
      CALIBRATION_FACTOR_TDS = newTDS;
      logger("API", "✓ CAMBIADO kfactor_tds: %.2f → %.2f", oldValue, CALIBRATION_FACTOR_TDS);
      updated = true;
    } else {
      logger("API", "- kfactor_tds sin cambios (%.2f)", CALIBRATION_FACTOR_TDS);
    }
  } else {
    logger("API", "- kfactor_tds no presente en respuesta");
  }

  // Actualizar timing si existe
  if (doc.containsKey("update_controller_time")) {
    unsigned long newUpdateTime = doc["update_controller_time"].as<unsigned long>();
    if (newUpdateTime >= 10000 && newUpdateTime != updateControllerTime) { // mínimo 10s
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
    if (newLoopTime >= 500 && newLoopTime != loopTime) { // mínimo 500ms
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

  // Actualizar productId si existe
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

  // Actualizar product_name si existe
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

  // Actualizar reset_pending si existe
  if (doc.containsKey("reset_pending")) {
    bool newResetPending = doc["reset_pending"].as<bool>();
    if (newResetPending != reset_pending) {
      bool oldValue = reset_pending;
      reset_pending = newResetPending;
      logger("API", "✓ CAMBIADO reset_pending: %s → %s", 
             oldValue ? "true" : "false", 
             reset_pending ? "true" : "false");
      updated = true;
      
      // Log especial si se solicita reinicio
      if (reset_pending) {
        logger("API", "⚠️  REINICIO PENDIENTE - Se ejecutará en el próximo ciclo");
      }
    } else {
      logger("API", "- reset_pending sin cambios (%s)", reset_pending ? "true" : "false");
    }
  } else {
    logger("API", "- reset_pending no presente en respuesta");
  }

  // Actualizar flush_time si existe
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

  // Verificar expiración del token
  if (response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
    logger("API", "Token expirado - será renovado en próxima llamada");
    jwtToken = "";
  }

  // Resumen final
  logger("API", "=== RESUMEN ===");
  if (updated) {
    logger("API", "✓ CONFIGURACIONES ACTUALIZADAS desde servidor");
  } else {
    logger("API", "- Sin cambios necesarios (valores ya sincronizados)");
  }
  logger("API", "=====================================");

  return json;
}

// ================== PATCH CONTROLLER (Reset Pending) ==================
bool patchControllerResetPending() {
  if (jwtToken.length() == 0) {
    logger("API", "Error: No hay token para PATCH reset_pending");
    return false;
  }

  logger("API", "Enviando PATCH para cambiar reset_pending a false...");

  // Preparar JSON para PATCH
  String patchData = "{";
  patchData += "\"_id\":\"" + String(controllerId) + "\",";
  patchData += "\"reset_pending\":false";
  patchData += "}";

  if (connectedWiFi) {
    // Usar HTTPClient para WiFi
    HTTPClient http;
    http.begin(controllerPatchUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);

    logger("API", "PATCH payload: %s", patchData.c_str());

    int httpCode = http.sendRequest("PATCH", patchData);
    
    if (httpCode > 0) {
      String response = http.getString();
      logger("API", "PATCH response code: %d", httpCode);
      logger("API", "PATCH response: %s", response.c_str());
      
      if (httpCode >= 200 && httpCode < 300) {
        logger("API", "✓ reset_pending cambiado a false exitosamente");
        http.end();
        return true;
      } else {
        logger("API", "Error en PATCH: código %d", httpCode);
      }
    } else {
      logger("API", "Error de conexión en PATCH: %d", httpCode);
    }
    http.end();

  } else if (connectedEthernet) {
    // Usar EthernetClient para Ethernet
    if (client.connect(serverIp, serverPort)) {
      logger("API", "Conectado al servidor para PATCH");
      
      // Construir request HTTP PATCH
      client.print("PATCH /api/v1.0/controllers/");
      client.print(controllerId);
      client.println(" HTTP/1.1");
      client.print("Host: ");
      client.print(serverIp);
      client.print(":");
      client.println(serverPort);
      client.println("Content-Type: application/json");
      client.print("Authorization: Bearer ");
      client.println(jwtToken);
      client.print("Content-Length: ");
      client.println(patchData.length());
      client.println("Connection: close");
      client.println();
      client.print(patchData);

      logger("API", "PATCH request enviado");

      // Leer respuesta
      unsigned long timeout = millis() + 5000;
      while (client.available() == 0 && millis() < timeout) {
        delay(50);
      }

      if (client.available()) {
        String response = "";
        while (client.available()) {
          response += (char)client.read();
        }
        logger("API", "PATCH response: %s", response.c_str());
        
        if (response.indexOf("200") >= 0 || response.indexOf("201") >= 0) {
          logger("API", "✓ reset_pending cambiado a false exitosamente");
          client.stop();
          return true;
        } else {
          logger("API", "Error en PATCH response");
        }
      } else {
        logger("API", "Timeout en PATCH response");
      }
      client.stop();
    } else {
      logger("API", "Error: No se pudo conectar para PATCH");
    }
  }

  logger("API", "Error: No se pudo cambiar reset_pending a false");
  return false;
}

// ================== ENVIAR DATOS (del código probado) ==================
void enviarDatos(const char* productIdParam, float flujoProd, float flujoRech, float tds, float temperature) {
  // 🔹 Obtener token si es necesario
  if (jwtToken == "") {
    logger("API", "Sin token - obteniendo token para enviarDatos...");
    obtenerToken();
    delay(200);
    if (jwtToken == "") {
      logger("API", "Error: No se pudo obtener token para enviarDatos");
      return; // No se pudo obtener token
    }
  }

  // 🔹 Calcular timestamps según loopTime
  unsigned long tiempo_fin_ms = millis();
  unsigned long tiempo_inicio_ms = (tiempo_fin_ms >= loopTime) ? tiempo_fin_ms - loopTime : 0;

  // 🔹 Crear JSON con ArduinoJson
  StaticJsonDocument<512> doc;
  doc["producto"] = productIdParam;

  JsonObject realData = doc.createNestedObject("real_data");
  realData["flujo_produccion"] = flujoProd;
  realData["flujo_rechazo"] = flujoRech;
  realData["tds"] = tds;
  realData["temperature"] = temperature;

  doc["tiempo_inicio"] = tiempo_inicio_ms;
  doc["tiempo_fin"] = tiempo_fin_ms;

  JsonObject pinStatus = doc.createNestedObject("pin_status");
  pinStatus["gpio15"] = digitalRead(FLOW_PIN_PROD);
  pinStatus["gpio4"] = digitalRead(FLOW_PIN_RECH);
  pinStatus["relay"] = RELAY_STATE ? 1 : 0;
  pinStatus["control_pin_1"] = digitalRead(CONTROL_PIN_1);
  pinStatus["control_pin_2"] = digitalRead(CONTROL_PIN_2);

  String json;
  serializeJson(doc, json);

  logger("DATA", "Enviando datos: Prod=%.2f L/m, Rech=%.2f L/m, TDS=%.0f ppm", 
         flujoProd, flujoRech, tds);

  if (connectedWiFi) {
    // 🔹 Enviar HTTP POST por WiFi usando HTTPClient
    HTTPClient http;
    http.begin("http://164.92.95.176:3009/api/v1.0/products/componentInput");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + jwtToken);
    http.setTimeout(5000);

    int httpCode = http.POST(json);
    
    if (httpCode > 0) {
      String response = http.getString();
      logger("DATA", "WiFi POST response code: %d", httpCode);
      
      if (httpCode >= 200 && httpCode < 300) {
        logger("DATA", "✓ Datos enviados exitosamente via WiFi");
      } else {
        logger("DATA", "Error enviando datos via WiFi: código %d", httpCode);
      }
      
      // Verificar expiración del token
      if (response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
        logger("API", "Token expirado - será renovado en próxima llamada");
        jwtToken = "";
      }
    } else {
      logger("DATA", "Error de conexión enviando datos via WiFi: %d", httpCode);
    }
    http.end();

  } else if (connectedEthernet) {
    // 🔹 Enviar HTTP POST por Ethernet usando EthernetClient
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      logger("DATA", "Error: No se pudo conectar al servidor para enviar datos");
      return;
    }

    String request = "";
    request += "POST /api/v1.0/products/componentInput HTTP/1.1\r\n";
    request += "Host: 164.92.95.176\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Authorization: Bearer " + jwtToken + "\r\n";
    request += "Content-Length: " + String(json.length()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += json;

    localClient.print(request);

    // 🔹 Leer respuesta del servidor
    String response = "";
    unsigned long timeout = millis();
    while (localClient.connected() && millis() - timeout < 5000) {
      while (localClient.available()) response += (char)localClient.read();
    }
    localClient.stop();

    if (response.length() > 0) {
      logger("DATA", "✓ Datos enviados exitosamente via Ethernet");
      
      // 🔹 Verificar expiración del token
      if (response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
        logger("API", "Token expirado - será renovado en próxima llamada");
        jwtToken = "";
      }
    } else {
      logger("DATA", "Error: No se recibió respuesta del servidor");
    }
  } else {
    logger("DATA", "Sin conexión de red - datos no enviados");
  }
}

// ================== DISPLAY FUNCTIONS ==================
void clearArea(int x, int y, int w, int h) {
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

void drawTankIcon(int x, int y, bool full) {
  int w = 40, h = 50;
  uint16_t color = full ? TFT_BLUE : TFT_LIGHTGREY;
  
  tft.drawRect(x + 5, y, w, h, TFT_BLACK);
  if (full) {
    tft.fillRect(x + 7, y + 10, w - 4, h - 12, color);
  }
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 5, y + 60);
  tft.print("Tanque");
}

void drawPumpIcon(int x, int y, bool state) {
  int radius = 25;
  uint16_t color = state ? TFT_ORANGE : TFT_DARKGREY;
  
  // Limpiar toda el área del icono primero
  tft.fillCircle(x + radius, y + radius, radius + 2, TFT_WHITE);
  
  // Dibujar círculo base
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  // Actualizar animación solo si está encendida
  if (state && millis() - lastPumpUpdate > 500) { // Mucho más lento: 500ms
    currentPumpSpeed = 0.3; // Velocidad fija y lenta
    pumpRotation += currentPumpSpeed;
    if (pumpRotation > 6.28) pumpRotation = 0;
    lastPumpUpdate = millis();
  } else if (!state) {
    pumpRotation = 0;
  }
  
  // Dibujar aspas
  float baseAngle = state ? pumpRotation : 0;
  for (int i = 0; i < 3; i++) {
    float angle = baseAngle + i * 2.094; // 120 grados
    
    // Aspas más gruesas y cortas para mejor visualización
    int x1 = x + radius + 12 * cos(angle);
    int y1 = y + radius + 12 * sin(angle);
    
    // Dibujar línea más gruesa
    tft.drawLine(x + radius, y + radius, x1, y1, TFT_WHITE);
    tft.drawLine(x + radius + 1, y + radius, x1 + 1, y1, TFT_WHITE);
    tft.drawLine(x + radius, y + radius + 1, x1, y1 + 1, TFT_WHITE);
    
    // Punta del aspa
    tft.fillCircle(x1, y1, 1, TFT_WHITE);
  }
  
  // Círculo central más grande
  tft.fillCircle(x + radius, y + radius, 4, TFT_BLACK);
  
  // Texto
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 8, y + 60);
  tft.print("Bomba");
}

void drawNetworkIcon(int x, int y, bool online, String type) {
  int radius = 25;
  uint16_t color;
  
  if (!online) {
    color = TFT_DARKGREY;
  } else if (type == "WiFi") {
    color = 0x07E0; // Verde verdadero para WiFi (RGB565: 0,255,0)
  } else {
    color = 0x07FF; // Azul celeste (cyan) para cable ethernet
  }
  
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  if (online) {
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
  } else {
    tft.drawLine(x + radius - 6, y + radius - 6, x + radius + 6, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius + 6, y + radius - 6, x + radius - 6, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius - 5, y + radius - 6, x + radius + 7, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius + 5, y + radius - 6, x + radius - 7, y + radius + 6, TFT_BLACK);
  }
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 12, y + 60);
  tft.print("Red");
}

// ====== FUNCIÓN PRINCIPAL PARA DIBUJAR PANTALLA COMPLETA ======
void drawCompleteDisplay() {
  // Limpiar pantalla completa
  tft.fillScreen(TFT_WHITE);
  
  // Calcular dimensiones para iconos
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
  
  // Dibujar todos los iconos
  drawPowerIcon(currentX, iconY, POWER_ON);
  currentX += iconWidth + spacing;
  
  drawTankIcon(currentX, iconY, TANK_FULL);
  currentX += iconWidth + spacing;
  
  drawPumpIcon(currentX, iconY, RELAY_STATE);
  currentX += iconWidth + spacing;
  
  drawNetworkIcon(currentX, iconY, CURRENT_CONNECTION_STATE, CONNECTION_TYPE);
  
  // Dibujar etiquetas estáticas
  int startY = 130;
  int lineH = 35;  // Reducido de 50 a 35 para que quepa todo
  int avgX = screenWidth * 0.80;
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
  tft.setCursor(10, startY);
  tft.print("Produccion:");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.setCursor(avgX, startY + 8);
  tft.printf("/ %.0fL", AVG_FLOW_PROD);
  
  tft.setTextColor(0xF800, TFT_WHITE); // Rojo puro (RGB565: 255,0,0)
  tft.setCursor(10, startY + lineH);
  tft.print("Rechazo:");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.setCursor(avgX, startY + lineH + 8);
  tft.printf("/ %.0fL", AVG_FLOW_RECH);
  
  tft.setTextColor(0xFD20, TFT_WHITE); // Naranja verdadero (RGB565: 255,165,0)
  tft.setCursor(10, startY + lineH * 2);
  tft.print("TDS:");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.setCursor(avgX, startY + lineH * 2 + 8);
  tft.printf("/ %.0f", AVG_TDS);
  
  // Agregar product_name después de TDS
  tft.setTextColor(TFT_PURPLE, TFT_WHITE);
  tft.setCursor(10, startY + lineH * 3);
  tft.print("Producto: ");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  if (product_name.length() > 0) {
    tft.printf("%s", product_name.c_str());
  } else {
    tft.print("Sin nombre");
  }
  
  // Forzar que se muestren todos los valores en redibujado completo
  displayInitialized = false;
  
  // Dibujar valores actuales
  updateDisplayValues();
  
  // Actualizar estado tracking DESPUÉS de dibujar
  lastPowerState = POWER_ON;
  lastTankState = TANK_FULL;
  lastRelayState = RELAY_STATE;
  lastConnectionState = CURRENT_CONNECTION_STATE;
  lastConnectionType = CONNECTION_TYPE;
  lastProductName = product_name;
}

// ====== FUNCIÓN PARA ACTUALIZAR SOLO VALORES DINÁMICOS ======
void updateDisplayValues() {
  int startY = 130;
  int lineH = 35;  // Reducido de 50 a 35 para que quepa todo
  int screenWidth = tft.width();
  int valueX = screenWidth * 0.45;
  int avgX = screenWidth * 0.80;
  
  // Actualizar valor de producción
  if (!displayInitialized || abs(CURRENT_FLOW_PROD - lastFlowProd) > 0.1) {
    clearArea(valueX, startY, avgX - valueX - 10, 30);
    tft.setTextSize(2);
    tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
    tft.setCursor(valueX, startY);
    tft.printf("%.2f L/m", CURRENT_FLOW_PROD);
    lastFlowProd = CURRENT_FLOW_PROD;
  }
  
  // Actualizar valor de rechazo
  if (!displayInitialized || abs(CURRENT_FLOW_RECH - lastFlowRech) > 0.1) {
    clearArea(valueX, startY + lineH, avgX - valueX - 10, 30);
    tft.setTextSize(2);
    tft.setTextColor(0xF800, TFT_WHITE); // Rojo puro
    tft.setCursor(valueX, startY + lineH);
    tft.printf("%.2f L/m", CURRENT_FLOW_RECH);
    lastFlowRech = CURRENT_FLOW_RECH;
  }
  
  // Actualizar valor de TDS
  if (!displayInitialized || abs(CURRENT_TDS - lastTDS) > 5.0) {
    clearArea(valueX, startY + lineH * 2, avgX - valueX - 10, 30);
    tft.setTextSize(2);
    tft.setTextColor(0xFD20, TFT_WHITE); // Naranja verdadero
    tft.setCursor(valueX, startY + lineH * 2);
    tft.printf("%.0f ppm", CURRENT_TDS);
    lastTDS = CURRENT_TDS;
  }
  
  // Actualizar product_name si cambió
  if (!displayInitialized || product_name != lastProductName) {
    clearArea(10, startY + lineH * 3, screenWidth - 20, 30);
    tft.setTextSize(2);
    tft.setTextColor(TFT_PURPLE, TFT_WHITE);
    tft.setCursor(10, startY + lineH * 3);
    tft.print("Producto: ");
    tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
    if (product_name.length() > 0) {
      tft.printf("%s", product_name.c_str());
    } else {
      tft.print("Sin nombre");
    }
    lastProductName = product_name;
  }
  
  // Actualizar contador de sincronización
  unsigned long timeSinceLastUpdate = (millis() - lastUpdateController) / 1000;
  unsigned long displayTime = timeSinceLastUpdate % (updateControllerTime / 1000);
  
  if (displayTime != lastSyncTime) {
    clearArea(10, 280, 300, 25);
    tft.setTextSize(2);
    tft.setTextColor(TFT_BLUE, TFT_WHITE);
    tft.setCursor(10, 280);
    tft.printf("Actualizado hace %lu segundos", displayTime);
    lastSyncTime = displayTime;
  }
  
  // Solo actualizar iconos que necesitan animación
  static unsigned long lastPumpAnimation = 0;
  if (RELAY_STATE && millis() - lastPumpAnimation > 500) {
    // Calcular posición de bomba
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




// ================== SYSTEM CONTROL ==================
// Variable para estado de relay del código probado
bool relayState = true;

void updateRelay() {
  // Lógica exacta del código probado (completo_inestable.ino)
  bool state27 = digitalRead(CONTROL_PIN_1);
  bool state25 = digitalRead(CONTROL_PIN_2);

  if (!state27 && !state25) {
    if (relayState) {
      relayState = false;
      digitalWrite(RELAY_PIN, HIGH); // Relay apagado (activo bajo)
      logger("RELAY", "Relay OFF - Control pins: 27=%d, 25=%d", state27, state25);
    }
  } else {
    if (!relayState) {
      relayState = true;
      digitalWrite(RELAY_PIN, LOW); // Relay encendido (activo bajo)
      logger("RELAY", "Relay ON - Control pins: 27=%d, 25=%d", state27, state25);
    }
  }
  
  // Sincronizar con variable global para display
  RELAY_STATE = relayState;
}

void updateSystemLogic() {
  // Usar la lógica de relay del código probado
  updateRelay();
  
  // Update tank level simulation (mantener lógica existente)
  static float tankLevel = 50.0;
  if (CURRENT_FLOW_PROD > 0) {
    tankLevel += 0.1;
  } else {
    tankLevel -= 0.05;
  }
  tankLevel = constrain(tankLevel, 0, 100);
  TANK_FULL = (tankLevel > 80);
  
  // Update connection state
  if (deviceIP != "0.0.0.0" && deviceIP != "") {
    CURRENT_CONNECTION_STATE = true;
    CONNECTION_TYPE = connectedWiFi ? "WiFi" : "Cable";
  } else {
    CURRENT_CONNECTION_STATE = false;
  }
}

// ================== INITIALIZATION FUNCTIONS ==================
void setupPins() {
  pinMode(TDS_PIN, INPUT);
  pinMode(FLOW_PIN_PROD, INPUT_PULLUP);
  pinMode(FLOW_PIN_RECH, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(CONTROL_PIN_1, INPUT_PULLUP); // Cambio a INPUT_PULLUP para leer estado
  pinMode(CONTROL_PIN_2, INPUT_PULLUP); // Cambio a INPUT_PULLUP para leer estado
}

void initializeTFT() {
  logger("TFT", "Inicializando pantalla...");
  
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  
  // Configure for 480x320
  tft.writecommand(0x2A);
  tft.writedata(0x00); tft.writedata(0x00);
  tft.writedata(0x01); tft.writedata(0xDF);
  
  tft.writecommand(0x2B);
  tft.writedata(0x00); tft.writedata(0x00);
  tft.writedata(0x01); tft.writedata(0x3F);
  
  tft.writecommand(0x36);
  tft.writedata(0x28);
  
  // Pantalla inicial
  tft.fillScreen(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(10, tft.height()/2 - 10);
  tft.print("Iniciando AquaTech...");
  delay(800);
  
  logger("TFT", "Pantalla inicializada correctamente");
}

void initializeSensors() {
  logger("SENSORS", "Configurando sensores...");
  
  // Configuración ADC para TDS
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);
  
  // Interrupciones para sensores de flujo
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_PROD), flowInterruptProd, RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_RECH), flowInterruptRech, RISING);
  
  // Estado inicial del relay (activo bajo en el código probado)
  relayState = false; // Inicializar como apagado
  digitalWrite(RELAY_PIN, HIGH); // HIGH = apagado (activo bajo)
  RELAY_STATE = relayState;
  
  logger("SENSORS", "Sensores configurados correctamente");
}

void initializeNetwork() {
  logger("NET", "Inicializando conectividad...");
  
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
  
  // Mostrar estado en pantalla
  tft.setCursor(10, tft.height()/2 + 20);
  tft.setTextColor(TFT_BLUE, TFT_WHITE);
  tft.print("Conectando red...");
  
  // Intentar Ethernet primero
  connectEthernet();
  
  if (!connectedEthernet) {
    logger("NET", "Ethernet no disponible, intentando WiFi guardado...");
    tft.setCursor(10, tft.height()/2 + 40);
    tft.setTextColor(TFT_ORANGE, TFT_WHITE);
    tft.print("Buscando WiFi...");
    
    if (!tryConnectSavedWiFi(8000)) {
      logger("NET", "Sin credenciales WiFi - esperando Bluetooth");
      tft.setCursor(10, tft.height()/2 + 60);
      tft.setTextColor(TFT_RED, TFT_WHITE);
      tft.print("Envia SSID,PASS por BT");
      deviceIP = "0.0.0.0";
      CURRENT_CONNECTION_STATE = false;
    }
  }
  
  updateNetworkStatus();
  logger("NET", "Inicialización de red completada - IP: %s", deviceIP.c_str());
}

// ================== SETUP ==================
void setup() {
  // Inicialización básica
  Serial.begin(115200);
  SerialBT.begin("AquaTech_Controller");
  logger("INFO", "=== INICIANDO AQUATECH CONTROLLER ===");
  
  // Configurar pines y sensores
  setupPins();
  initializeSensors();
  
  // Inicializar pantalla
  initializeTFT();
  
  // Configurar conectividad de red
  initializeNetwork();
  
  // Obtener configuración remota si hay conexión
  if (connectedEthernet || connectedWiFi) {
    if (testServerConnectivity()) {
      logger("API", "Obteniendo configuración inicial del servidor...");
      obtenerToken();
      if (jwtToken != "") {
        String controllerData = getControllerById();
        // No hacer nada con controllerData aquí, solo obtener configuraciones
      }
    } else {
      logger("API", "Servidor no accesible en setup - usando configuración por defecto");
    }
  } else {
    logger("API", "Sin conexión - usando configuración por defecto");
  }
  
  // Mostrar interfaz principal
  drawCompleteDisplay();
  
  logger("INFO", "=== SETUP COMPLETADO === IP: %s", deviceIP.c_str());
}

// ================== MAIN LOOP ==================
void loop() {
  unsigned long now = millis();
  
  // Verificar si hay reinicio pendiente
  if (reset_pending) {
    logger("SYSTEM", "🔄 REINICIO REMOTO SOLICITADO...");
    logger("SYSTEM", "Cambiando reset_pending a false en servidor antes de reiniciar");
    
    // Intentar cambiar reset_pending a false en el servidor
    bool patchSuccess = patchControllerResetPending();
    
    if (patchSuccess) {
      logger("SYSTEM", "✓ reset_pending actualizado en servidor");
      logger("SYSTEM", "🔄 EJECUTANDO REINICIO en 3 segundos...");
      delay(3000); // Dar tiempo para que se complete la operación
      ESP.restart();
    } else {
      logger("SYSTEM", "⚠️ Error actualizando servidor - reiniciando de todos modos");
      logger("SYSTEM", "🔄 EJECUTANDO REINICIO en 2 segundos...");
      delay(2000);
      ESP.restart();
    }
  }
  
  // Leer sensores continuamente
  readTDSSensor();
  updateFlowRates();
  updateSystemLogic();
  
  // Estrategia de pantalla y API: cada updateControllerTime
  if (now - lastUpdateController >= updateControllerTime) {
    // Intentar reconexión de red si es necesario
    if (!connectedEthernet && !connectedWiFi) {
      logger("NET", "Reintentando conexión de red...");
      connectEthernet();
      
      if (!connectedEthernet) {
        tryConnectSavedWiFi(5000); // timeout más corto en loop
      }
      
      updateNetworkStatus();
    }
    
    // Obtener datos del controller si hay conexión
    if (connectedEthernet || connectedWiFi) {
      // Test de conectividad antes de intentar API
      if (testServerConnectivity()) {
        logger("API", "Actualizando datos del controller...");
        String controllerData = getControllerById();
        // Las configuraciones ya se actualizan dentro de getControllerById()
  } else {
        logger("API", "Servidor no accesible - saltando actualización");
      }
    }
    
    drawCompleteDisplay();
    lastUpdateController = now;
    logger("DISPLAY", "Ciclo completado - Red: %s - Token: %s", 
           CURRENT_CONNECTION_STATE ? "ON" : "OFF", 
           jwtToken != "" ? "OK" : "NO");
  }
  
  // Actualizar solo valores cada loopTime
  if (now - lastLoop >= loopTime) {
    updateDisplayValues();
    
    // Enviar datos al servidor si hay conexión (del código probado)
    if (connectedEthernet || connectedWiFi) {
      enviarDatos(productId.c_str(), CURRENT_FLOW_PROD, CURRENT_FLOW_RECH, CURRENT_TDS, 25.6);
    }
    
    lastLoop = now;
  }
  
  // Manejar credenciales WiFi por Bluetooth si no hay conexión
  if (!connectedEthernet && !connectedWiFi) {
    handleBluetoothWiFi();
  } else {
    // Si hay red, todavía aceptar CLEAR_WIFI por BT
    if (SerialBT.available()) {
      String cmd = SerialBT.readStringUntil('\n');
      cmd.trim();
      if (cmd.equalsIgnoreCase("CLEAR_WIFI")) {
        logger("BT", "Comando CLEAR_WIFI recibido");
        WiFi.disconnect(true, true);
        delay(400);
        ESP.restart();
      }
    }
  }
  
  // Delay mínimo
  delay(10);
}
