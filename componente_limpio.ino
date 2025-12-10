// ================== AQUATECH CONTROLLER - VERSIÓN LIMPIA ==================
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
#define PIN_MISO 19
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
const int TDS_PIN = 32;
const int FLOW_PIN_PROD = 15;
const int FLOW_PIN_RECH = 4;
const int RELAY_PIN = 16;
const int RELAY_FLUSH_PIN = 17;  // ✅ Relay para flush programado
const int CONTROL_PIN_1 = 27;  // Válvula
const int CONTROL_PIN_2 = 25;  // Tanque
const int FLUSH_BUTTON_PIN = 12;  // ✅ Botón de flush manual (LOW = activo)

// ================== SENSOR CONFIGURATION ==================
float CALIBRATION_FACTOR_TDS = 1.58f;
const float ALPHA = 0.1f; // Filtro exponencial

// ⚠️ IMPORTANTE: FLOW_FACTOR se actualiza desde la API
// Representa: K-factor del sensor calibrado para 1 segundo
// Ejemplo: Si FLOW_FACTOR = 24, significa que 24 pulsos/segundo = 1 L/min
// Cálculo: (pulsos_por_segundo / FLOW_FACTOR) = L/min
// El código normaliza automáticamente los pulsos a "por segundo"
float FLOW_FACTOR = 24.0f;

// ================== SYSTEM STATE ==================
float CURRENT_TDS = 75.0f;
float CURRENT_FLOW_PROD = 0.0f;
float CURRENT_FLOW_RECH = 0.0f;
float CURRENT_VOLUME_PROD = 0.0f;
float CURRENT_VOLUME_RECH = 0.0f;
float AVG_FLOW_PROD = 150.0f;
float AVG_FLOW_RECH = 130.0f;
float AVG_TDS = 75.0f;

bool CURRENT_CONNECTION_STATE = false;
bool RELAY_STATE = false;
bool RELAY_FLUSH_STATE = false;  // ✅ Estado del relay de flush
bool TANK_FULL = false;
bool POWER_ON = true;
String product_name = "AquaTech Controller";
bool reset_pending = false;

// ================== FLUSH CONFIGURATION ==================
unsigned long FLUSH_INTERVAL = 14400000;  // ✅ Intervalo de flush (14400000 ms = 4 horas) - CONFIGURABLE desde API
const unsigned long FLUSH_DURATION = 30000;  // Duración del flush (30 segundos)
unsigned long flush_time = 14400000;     // Variable legacy para compatibilidad con API (sincronizada con FLUSH_INTERVAL)
unsigned long lastFlushTime = 0;      // Timestamp del último flush
bool isFlushActive = false;           // Flag para indicar si estamos en modo flush

// ================== NETWORK STATE ==================
bool connectedEthernet = false;
bool connectedWiFi = false;
String deviceIP = "0.0.0.0";
String jwtToken = "";
String productId = "69081e0cf0da5b3801500b11";
String CONNECTION_TYPE = "Sin Red";
const char* controllerId = "690825bd1bd0277267429c06";

// URLs para HTTPClient (WiFi)
const char* loginUrl = "http://164.92.95.176:3009/api/v1.0/auth/login";
const char* controllerUrl = "http://164.92.95.176:3009/api/v1.0/controllers/690825bd1bd0277267429c06"; 
const char* dataUrl = "http://164.92.95.176:3009/api/v1.0/products/componentInput";

// ================== FLOW SENSORS ==================
volatile int FLOW_PULSE_PROD = 0;
volatile int FLOW_PULSE_RECH = 0;

// ================== PUMP ANIMATION ==================
float pumpRotation = 0.0;
float currentPumpSpeed = 0.15;
unsigned long lastPumpUpdate = 0;

// ================== DISPLAY STATE TRACKING ==================
static bool lastPowerState = true;
static bool lastTankState = false;
static bool lastRelayState = false;
static bool lastConnectionState = false;
static String lastConnectionType = "";
static float lastTDS = -999;
static float lastFlowProd = -999;
static float lastFlowRech = -999;
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
void updateTankStatus();
void updateTankIcon();
void updatePumpIcon();
void drawStatusMessage(String message, uint16_t color);
void clearStatusMessage();
void drawBluetoothIcon(int x, int y);
void updateNetworkIconDuringConnection(String connectionType);
void drawNetworkIconWithLoading(int x, int y, String type);
bool testServerConnectivity();
void obtenerToken();
String getControllerById();
void enviarDatos(const char* productIdParam, float flujoProd, float flujoRech, float tds, float temperature);
void executeFlush();
void updateFlushRelay();
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
// Variables para TDS
const int NUM_SAMPLES = 50;
float tdsFiltered = 0.0f;
float lastVoltage = 0.0f;

float readAverageVoltage() {
  long sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    sum += analogRead(TDS_PIN);
    delay(2);
  }
  float adcAverage = sum / (float)NUM_SAMPLES;
  return adcAverage * 3.3 / 4095.0;
}

void readTDSSensor() {
  static bool firstRead = true;
  
  float voltage = readAverageVoltage();
  lastVoltage = voltage;

  const float V_MIN = 0.05;
  float tds_ppm;

  if (voltage < V_MIN) {
    tds_ppm = 35.0 + (voltage / V_MIN) * (CALIBRATION_FACTOR_TDS * voltage * 100.0);
  } else {
    tds_ppm = voltage * CALIBRATION_FACTOR_TDS;
  }

  tdsFiltered = tdsFiltered * (1 - ALPHA) + tds_ppm * ALPHA;
  
  if (firstRead) {
    CURRENT_TDS = tdsFiltered;
    firstRead = false;
  } else {
    CURRENT_TDS = tdsFiltered;
  }
  
  logWithTimestamp("TDS", "TDS leído: %.0f ppm (voltaje: %.3fV)", CURRENT_TDS, voltage);
}

// Variables para flujo
float flowRateProd = 0.0;
float flowRateRech = 0.0;
float volumenProd = 0.0;
float volumenRech = 0.0;
unsigned long lastFlowCalc = 0;

void updateFlowRates() {
  unsigned long now = millis();
  unsigned long deltaTime = now - lastFlowCalc;

  if (deltaTime >= loopTime) {
    float deltaMinutes = deltaTime / 60000.0;

    // 🔍 DEBUG: Log de valores antes del cálculo
    logWithTimestamp("FLOW_DEBUG", "=== INICIO CÁLCULO DE FLUJO ===");
    logWithTimestamp("FLOW_DEBUG", "Pulsos acumulados - Prod: %d, Rech: %d", FLOW_PULSE_PROD, FLOW_PULSE_RECH);
    logWithTimestamp("FLOW_DEBUG", "FLOW_FACTOR actual: %.2f", FLOW_FACTOR);
    logWithTimestamp("FLOW_DEBUG", "Tiempo transcurrido: %lu ms (%.4f minutos)", deltaTime, deltaMinutes);
    logWithTimestamp("FLOW_DEBUG", "loopTime configurado: %lu ms", loopTime);
    
    // ⚠️ CORRECCIÓN FINAL: El FLOW_FACTOR está calibrado para 1 SEGUNDO
    // Por lo tanto: pulsos_por_segundo / FLOW_FACTOR = L/min
    // Necesitamos normalizar los pulsos a "por segundo"
    float pulsosProducidosPorSegundo = (float)FLOW_PULSE_PROD / (deltaTime / 1000.0);
    float pulsosRechazadosPorSegundo = (float)FLOW_PULSE_RECH / (deltaTime / 1000.0);
    
    // Ahora calcular flujo con los pulsos normalizados
    flowRateProd = pulsosProducidosPorSegundo / FLOW_FACTOR;  // L/min
    flowRateRech = pulsosRechazadosPorSegundo / FLOW_FACTOR;  // L/min
    
    logWithTimestamp("FLOW_DEBUG", "Pulsos/seg normalizados - Prod: %.2f p/s, Rech: %.2f p/s", 
                     pulsosProducidosPorSegundo, pulsosRechazadosPorSegundo);
    logWithTimestamp("FLOW_DEBUG", "Flujo calculado - Prod: %.4f L/m, Rech: %.4f L/m", 
                     flowRateProd, flowRateRech);
    
    // Calcular volumen para este intervalo
    float volumenIntervaloProd = flowRateProd * deltaMinutes;
    float volumenIntervaloRech = flowRateRech * deltaMinutes;
    
    logWithTimestamp("FLOW_DEBUG", "Volumen en este intervalo - Prod: %.4f L, Rech: %.4f L", 
                     volumenIntervaloProd, volumenIntervaloRech);

    // Acumular volumen total
    volumenProd += volumenIntervaloProd;
    volumenRech += volumenIntervaloRech;
    
    logWithTimestamp("FLOW_DEBUG", "Volumen acumulado total - Prod: %.2f L, Rech: %.2f L", 
                     volumenProd, volumenRech);
    
    // 🔍 ANÁLISIS: ¿Cuál sería el flujo si escalamos por tiempo?
    float flowEscaladoProd = flowRateProd / deltaMinutes;
    float flowEscaladoRech = flowRateRech / deltaMinutes;
    logWithTimestamp("FLOW_DEBUG", "Si escaláramos por tiempo: Prod: %.2f L/m, Rech: %.2f L/m (IGNORAR)", 
                     flowEscaladoProd, flowEscaladoRech);

    CURRENT_FLOW_PROD = flowRateProd;
    CURRENT_FLOW_RECH = flowRateRech;
    CURRENT_VOLUME_PROD = volumenProd;
    CURRENT_VOLUME_RECH = volumenRech;

    FLOW_PULSE_PROD = 0;
    FLOW_PULSE_RECH = 0;
    lastFlowCalc = now;
    
    logWithTimestamp("FLOW", "📊 RESULTADO FINAL - Prod: %.2f L/m, Rech: %.2f L/m | Tiempo: %lu ms | Factor: %.2f", 
                     flowRateProd, flowRateRech, deltaTime, FLOW_FACTOR);
    logWithTimestamp("FLOW_DEBUG", "=== FIN CÁLCULO DE FLUJO ===");
  }
}

void IRAM_ATTR flowInterruptProd() { FLOW_PULSE_PROD++; }
void IRAM_ATTR flowInterruptRech() { FLOW_PULSE_RECH++; }

// ================== RELAY CONTROL ==================
bool relayState = false;

void updateRelay() {
  bool state27 = digitalRead(CONTROL_PIN_1);  // Válvula
  bool state25 = digitalRead(CONTROL_PIN_2);  // Tanque
  bool previousRelayState = relayState;
  
  // ✅ Verificar botón de flush manual (pin 12 en LOW = activo)
  bool flushButtonActive = (digitalRead(FLUSH_BUTTON_PIN) == LOW);
  
  // Verificar si relay de flush está activo (programado O manual)
  bool flushOverride = isFlushActive || flushButtonActive;

  // ✅ CONTROL DEL RELAY DE FLUSH (Pin 17)
  // Activar relay de flush si hay flush programado O botón manual presionado
  if (flushOverride) {
    if (!RELAY_FLUSH_STATE) {
      digitalWrite(RELAY_FLUSH_PIN, HIGH);  // ✅ HIGH = ON
      RELAY_FLUSH_STATE = true;
      if (flushButtonActive) {
        logWithTimestamp("RELAY", "🔄 FLUSH MANUAL: Relay Flush (Pin 17) ON - Botón presionado");
      }
    }
  } else {
    // Solo desactivar si NO hay flush programado activo (el botón manual tiene prioridad)
    if (RELAY_FLUSH_STATE && !isFlushActive) {
      digitalWrite(RELAY_FLUSH_PIN, LOW);   // ✅ LOW = OFF
      RELAY_FLUSH_STATE = false;
      if (!flushButtonActive) {
        logWithTimestamp("RELAY", "🔄 FLUSH MANUAL: Relay Flush (Pin 17) OFF - Botón liberado");
      }
    }
  }

  // ✅ LÓGICA CORREGIDA: 
  // Válvulas conectadas a GND (LOW) = ACTIVAS → Relay ON
  // Válvulas desconectadas (HIGH) = INACTIVAS → Relay OFF
  
  if ((!state27 && !state25) || flushOverride) {
    // AMBAS válvulas conectadas a GND (activas) O flush activo → Relay ON
    if (!relayState) {
      relayState = true;
      digitalWrite(RELAY_PIN, HIGH);  // ✅ HIGH = ON
      
      if (flushOverride) {
        if (flushButtonActive) {
          logWithTimestamp("RELAY", "🔄 CAMBIO: Relay ON - FORZADO por FLUSH MANUAL (Botón Pin 12)");
        } else {
          logWithTimestamp("RELAY", "🔄 CAMBIO: Relay ON - FORZADO por FLUSH PROGRAMADO");
        }
      } else {
        logWithTimestamp("RELAY", "🔄 CAMBIO: Relay ON - Ambas válvulas activas (27=%d, 25=%d)",
                         state27, state25);
      }
    }
  } else {
    // No ambas válvulas activas Y sin flush activo → Relay OFF
    if (relayState) {
      relayState = false;
      digitalWrite(RELAY_PIN, LOW);  // ✅ LOW = OFF
      logWithTimestamp("RELAY", "🔄 CAMBIO: Relay OFF - Falta al menos una válvula (27=%d, 25=%d)",
                       state27, state25);
    }
  }
  
  if (relayState != previousRelayState) {
    logWithTimestamp("RELAY", "Estado final: %s (Pin 16: %s)", 
           relayState ? "ON" : "OFF", 
           digitalRead(RELAY_PIN) == HIGH ? "HIGH" : "LOW");
  }
  
  RELAY_STATE = relayState;
}

void updateSystemLogic() {
  updateRelay();
  
  // Actualizar estado de conexión real
  updateNetworkStatus();
}

// ================== FLUSH RELAY FUNCTIONS ==================
void executeFlush() {
  logger("FLUSH", "═══════════════════════════════════════");
  logger("FLUSH", "🔄 INICIANDO FLUSH - Duración: 30 segundos");
  logger("FLUSH", "═══════════════════════════════════════");
  
  // Activar modo flush
  isFlushActive = true;
  logger("FLUSH", "✅ Flag isFlushActive = true");
  
  // ✅ Activar Relay 2 (Flush) - HIGH = ON
  digitalWrite(RELAY_FLUSH_PIN, HIGH);
  RELAY_FLUSH_STATE = true;
  delay(50);
  int pin17Check = digitalRead(RELAY_FLUSH_PIN);
  logger("FLUSH", "✅ Relay FLUSH (Pin 17): %s → %s",
         pin17Check == HIGH ? "HIGH" : "LOW",
         pin17Check == HIGH ? "ON ✓" : "OFF ❌");
  
  // ✅ Forzar activación del Relay 1 (Bomba) mediante updateRelay()
  // updateRelay() detectará isFlushActive=true y forzará el relay a ON
  logger("FLUSH", "🔄 Forzando Relay 1 (Bomba) mediante isFlushActive...");
  updateRelay();
  delay(50);
  int pin16Check = digitalRead(RELAY_PIN);
  logger("FLUSH", "✅ Relay BOMBA (Pin 16): %s → %s",
         pin16Check == HIGH ? "HIGH" : "LOW",
         pin16Check == HIGH ? "ON ✓" : "OFF ❌");
  
  // Verificar que ambos están encendidos
  if (pin16Check == HIGH && pin17Check == HIGH) {
    logger("FLUSH", "✅ ✅ AMBOS RELAYS ACTIVADOS - FLUSH EN PROGRESO");
  } else {
    logger("FLUSH", "⚠️ ERROR: No todos los relays están activos");
  }
  
  // Actualizar icono de bomba
  updatePumpIcon();
  
  // Esperar 30 segundos manteniendo AMBOS relays activos
  unsigned long startFlush = millis();
  unsigned long lastSecondLog = 0;
  
  logger("FLUSH", "⏱️ Manteniendo AMBOS relays activos por 30 segundos...");
  
  while (millis() - startFlush < FLUSH_DURATION) {
    unsigned long now = millis();
    
    // ⚠️ CRÍTICO: Mantener AMBOS pines en HIGH cada iteración
    digitalWrite(RELAY_PIN, HIGH);        // Relay 1 ON
    digitalWrite(RELAY_FLUSH_PIN, HIGH);  // Relay 2 ON
    
    // Log cada segundo (primeros 5 y últimos 5 segundos)
    unsigned long elapsedSeconds = (now - startFlush) / 1000;
    if (now - lastSecondLog >= 1000) {
      if (elapsedSeconds < 5 || elapsedSeconds >= 25) {
        int p16 = digitalRead(RELAY_PIN);
        int p17 = digitalRead(RELAY_FLUSH_PIN);
        logger("FLUSH", "⏱️ Seg %lu | Pin16: %s | Pin17: %s | Estado: %s", 
               elapsedSeconds,
               p16 == HIGH ? "ON ✓" : "OFF ❌",
               p17 == HIGH ? "ON ✓" : "OFF ❌",
               (p16 == HIGH && p17 == HIGH) ? "✓ AMBOS ON" : "❌ ERROR");
      }
      lastSecondLog = now;
    }
    
    // Mantener funciones críticas durante el flush
    updateTankStatus();
    
    // Actualizar display cada 500ms
    static unsigned long lastDisplayUpdate = 0;
    if (now - lastDisplayUpdate >= 500) {
      updateDisplayValues();
      lastDisplayUpdate = now;
    }
    
    delay(50);
  }
  
  logger("FLUSH", "═══════════════════════════════════════");
  logger("FLUSH", "✅ 30 SEGUNDOS COMPLETADOS - Desactivando flush");
  logger("FLUSH", "═══════════════════════════════════════");
  
  // Desactivar Relay 2 (Flush) - LOW = OFF
  digitalWrite(RELAY_FLUSH_PIN, LOW);
  RELAY_FLUSH_STATE = false;
  delay(50);
  int pin17Final = digitalRead(RELAY_FLUSH_PIN);
  logger("FLUSH", "✅ Relay FLUSH desactivado: %s → %s",
         pin17Final == LOW ? "LOW" : "HIGH",
         pin17Final == LOW ? "OFF ✓" : "ON ❌");
  
  // Desactivar modo flush
  isFlushActive = false;
  logger("FLUSH", "✅ Flag isFlushActive = false");
  
  // Restaurar lógica normal del Relay 1 (según válvulas)
  logger("FLUSH", "🔄 Restaurando lógica normal del Relay 1...");
  updateRelay();
  delay(50);
  int pin16Final = digitalRead(RELAY_PIN);
  logger("FLUSH", "📊 Relay 1 final: %s → %s (según válvulas)",
         pin16Final == HIGH ? "HIGH" : "LOW",
         pin16Final == HIGH ? "ON" : "OFF");
  
  // Actualizar icono de bomba
  updatePumpIcon();
  
  logger("FLUSH", "✅ FLUSH COMPLETADO - Regreso a operación normal");
  logger("FLUSH", "═══════════════════════════════════════");
  
  lastFlushTime = millis();
}

void updateFlushRelay() {
  // No ejecutar si ya hay un flush en proceso
  if (isFlushActive) {
    return;
  }
  
  unsigned long now = millis();
  unsigned long timeSinceLastFlush = now - lastFlushTime;
  
  // Log cada 10 segundos para monitoreo
  static unsigned long lastLog = 0;
  if (now - lastLog >= 10000) {
    unsigned long remaining = (FLUSH_INTERVAL > timeSinceLastFlush) ? (FLUSH_INTERVAL - timeSinceLastFlush) : 0;
    logger("FLUSH_MONITOR", "Próximo flush en %lu seg (Intervalo: %lu seg)",
           remaining / 1000,
           FLUSH_INTERVAL / 1000);
    lastLog = now;
  }
  
  // Ejecutar flush cuando se cumpla el intervalo
  if (timeSinceLastFlush >= FLUSH_INTERVAL) {
    logger("FLUSH_MONITOR", "⏰ INTERVALO ALCANZADO - Iniciando flush");
    executeFlush();
  }
}

// ================== NETWORK FUNCTIONS ==================
void initializeNetwork() {
  logger("NET", "🌐 INICIANDO CONECTIVIDAD DE RED...");
  logWithTimestamp("NET", "=== INICIO DE INICIALIZACIÓN DE RED ===");
  
  // ⚠️ IMPORTANTE: Evaluar estado del relay ANTES de iniciar red
  logger("NET", "Evaluando estado inicial del relay...");
  updateRelay();
  logger("NET", "Relay inicial: %s", RELAY_STATE ? "ON" : "OFF");
  
  // Mostrar tabla inmediatamente
  drawCompleteDisplay();
  
  // Mostrar mensaje de estado inicial
  drawStatusMessage("Iniciando AquaTech Controller...", TFT_BLUE);
  delay(500); // Reducido de 1000ms a 500ms
  
  // Actualizar relay durante delays
  updateRelay();
  
  // Mostrar estado de conexión por cable
  drawStatusMessage("Intentando conectar usando cable...", TFT_BLUE);
  updateNetworkIconDuringConnection("Cable");
  
  // Intentar Ethernet primero
  logger("NET", "Intentando conexión Ethernet...");
  
  // Actualizar relay antes de conectar Ethernet
  updateRelay();
  
  connectEthernet();
  
  // Actualizar relay después de conectar Ethernet
  updateRelay();
  
  if (connectedEthernet) {
    logger("NET", "✅ Ethernet conectado exitosamente");
    drawStatusMessage("Cable OK - IP: " + deviceIP, TFT_GREEN);
    delay(1000); // Reducido de 2000ms a 1000ms
    updateRelay();
  } else {
    logger("NET", "❌ Ethernet falló - continuando con WiFi");
  }
  
  if (!connectedEthernet) {
    logger("NET", "Ethernet no disponible, intentando WiFi guardado (3s timeout)...");
    drawStatusMessage("Intentando WiFi...", TFT_ORANGE);
    updateNetworkIconDuringConnection("WiFi");
    
    // Actualizar relay antes de intentar WiFi
    updateRelay();
    
    if (!tryConnectSavedWiFi(3000)) {
      logger("NET", "Sin credenciales WiFi - esperando Bluetooth");
      drawStatusMessage("Esperando BT...", TFT_RED);
      updateNetworkIconDuringConnection("Bluetooth");
      deviceIP = "0.0.0.0";
      CURRENT_CONNECTION_STATE = false;
    } else {
      drawStatusMessage("WiFi OK - IP: " + deviceIP, TFT_GREEN);
      delay(1000); // Reducido de 2000ms a 1000ms
    }
    
    // Actualizar relay después de WiFi
    updateRelay();
  }
  
  updateNetworkStatus();
  logWithTimestamp("NET", "=== INICIALIZACIÓN DE RED COMPLETADA ===");
  logger("NET", "Estado final - IP: %s, Ethernet: %s, WiFi: %s, Relay: %s", 
         deviceIP.c_str(), 
         connectedEthernet ? "true" : "false",
         connectedWiFi ? "true" : "false",
         RELAY_STATE ? "ON" : "OFF");
  
  // Limpiar mensaje de estado después de 2 segundos (reducido de 3s)
  delay(2000);
  updateRelay(); // Actualizar una última vez antes de terminar
  clearStatusMessage();
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

  // 🔍 LOG DE RESPUESTA CRUDA COMPLETA
  logger("API", "=== RESPUESTA CRUDA DEL SERVIDOR ===");
  logger("API", "Longitud de JSON: %d caracteres", json.length());
  
  // Dividir el JSON en partes para loguear (Arduino tiene límite de buffer)
  int chunkSize = 200;
  for (int i = 0; i < json.length(); i += chunkSize) {
    String chunk = json.substring(i, min(i + chunkSize, (int)json.length()));
    logger("API", "JSON[%d-%d]: %s", i, min(i + chunkSize, (int)json.length()), chunk.c_str());
  }
  logger("API", "=== FIN RESPUESTA CRUDA ===");

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    logger("API", "❌ Error: No se pudo parsear JSON del controller");
    logger("API", "Error de deserialización: %s", err.c_str());
    return json;
  }

  // Log de datos completos recibidos del servidor
  logger("API", "=== DATOS RECIBIDOS DEL CONTROLLER (PARSEADOS) ===");
  
  // 🔍 LISTAR TODOS LOS CAMPOS DEL JSON (incluso los que no conocemos)
  logger("API", "Campos encontrados en el JSON del controller:");
  JsonObject obj = doc.as<JsonObject>();
  int fieldCount = 0;
  for (JsonPair kv : obj) {
    fieldCount++;
    const char* key = kv.key().c_str();
    
    // Intentar determinar el tipo y mostrar el valor
    if (kv.value().is<int>() || kv.value().is<long>()) {
      logger("API", "  [%d] %s: %ld (entero)", fieldCount, key, kv.value().as<long>());
    } else if (kv.value().is<float>() || kv.value().is<double>()) {
      logger("API", "  [%d] %s: %.4f (decimal)", fieldCount, key, kv.value().as<float>());
    } else if (kv.value().is<bool>()) {
      logger("API", "  [%d] %s: %s (booleano)", fieldCount, key, kv.value().as<bool>() ? "true" : "false");
    } else if (kv.value().is<const char*>()) {
      logger("API", "  [%d] %s: '%s' (texto)", fieldCount, key, kv.value().as<const char*>());
    } else {
      logger("API", "  [%d] %s: (tipo desconocido)", fieldCount, key);
    }
  }
  logger("API", "Total de campos: %d", fieldCount);
  logger("API", "");
  
  // Mostrar campos conocidos con formato específico
  logger("API", "=== CAMPOS CONOCIDOS DEL CONTROLADOR ===");
  if (doc.containsKey("kfactor_flujo")) {
    logger("API", "✓ kfactor_flujo: %.2f", doc["kfactor_flujo"].as<float>());
  } else {
    logger("API", "✗ kfactor_flujo: NO PRESENTE");
  }
  if (doc.containsKey("kfactor_tds")) {
    logger("API", "✓ kfactor_tds: %.2f", doc["kfactor_tds"].as<float>());
  } else {
    logger("API", "✗ kfactor_tds: NO PRESENTE");
  }
  if (doc.containsKey("update_controller_time")) {
    logger("API", "✓ update_controller_time: %lu ms", doc["update_controller_time"].as<unsigned long>());
  } else {
    logger("API", "✗ update_controller_time: NO PRESENTE");
  }
  if (doc.containsKey("loop_time")) {
    logger("API", "✓ loop_time: %lu ms", doc["loop_time"].as<unsigned long>());
  } else {
    logger("API", "✗ loop_time: NO PRESENTE");
  }
  if (doc.containsKey("productId")) {
    logger("API", "✓ productId: %s", doc["productId"].as<String>().c_str());
  } else {
    logger("API", "✗ productId: NO PRESENTE");
  }
  if (doc.containsKey("product_name")) {
    logger("API", "✓ product_name: %s", doc["product_name"].as<String>().c_str());
  } else {
    logger("API", "✗ product_name: NO PRESENTE");
  }
  if (doc.containsKey("reset_pending")) {
    logger("API", "✓ reset_pending: %s", doc["reset_pending"].as<bool>() ? "true" : "false");
  } else {
    logger("API", "✗ reset_pending: NO PRESENTE");
  }
  if (doc.containsKey("flush_time")) {
    unsigned long apiFlushTime = doc["flush_time"].as<unsigned long>();
    logger("API", "✓ flush_time: %lu ms (%.0f seg)", apiFlushTime, apiFlushTime / 1000.0);
  } else {
    logger("API", "✗ flush_time: NO PRESENTE");
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
  logger("API", "Local - flush_time: %lu ms (%.0f segundos)", flush_time, flush_time / 1000.0);
  
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

  // Actualizar flush_time (intervalo de flush) si existe
  if (doc.containsKey("flush_time")) {
    unsigned long newFlushTime = doc["flush_time"].as<unsigned long>();
    const unsigned long MIN_FLUSH_INTERVAL = 3600000;  // Mínimo 1 hora (3600000 ms)
    
    if (newFlushTime >= MIN_FLUSH_INTERVAL && newFlushTime != flush_time) {
      unsigned long oldValue = flush_time;
      flush_time = newFlushTime;
      FLUSH_INTERVAL = newFlushTime;  // ✅ Sincronizar con FLUSH_INTERVAL
      logger("API", "✓ CAMBIADO flush_time (intervalo): %lu → %lu ms (%.0f horas)",
             oldValue, flush_time, flush_time / 3600000.0);
      updated = true;
    } else if (newFlushTime < MIN_FLUSH_INTERVAL) {
      logger("API", "⚠️ flush_time ignorado: %lu ms (mínimo %lu ms = 1 hora)", newFlushTime, MIN_FLUSH_INTERVAL);
      logger("API", "   Manteniendo valor local: %lu ms (%.0f horas)", FLUSH_INTERVAL, FLUSH_INTERVAL / 3600000.0);
    } else {
      logger("API", "- flush_time sin cambios (%lu ms = %.0f horas)", flush_time, flush_time / 3600000.0);
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

void enviarDatos(const char* productIdParam, float flujoProd, float flujoRech, float tds, float temperature) {
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
  logWithTimestamp("API", "Datos - Prod: %.2f L/m, Rech: %.2f L/m, TDS: %.2f ppm, Temp: %.2f°C", 
                   flujoProd, flujoRech, tds, temperature);

  String jsonPayload = "";
  jsonPayload += "{";
  jsonPayload += "\"productId\":\"" + String(productIdParam) + "\",";
  jsonPayload += "\"flujo_prod\":" + String(flujoProd, 2) + ",";
  jsonPayload += "\"flujo_rech\":" + String(flujoRech, 2) + ",";
  jsonPayload += "\"tds\":" + String(tds, 2) + ",";
  jsonPayload += "\"temperature\":" + String(temperature, 2) + ",";
  jsonPayload += "\"timestamp\":\"" + String(millis()) + "\"";
  jsonPayload += "}";

  logWithTimestamp("API", "Payload JSON: %s", jsonPayload.c_str());

  if (connectedWiFi) {
    // Usar HTTPClient para WiFi
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
    // Usar EthernetClient para Ethernet
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

    // Leer respuesta
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

  // Verificar expiración del token
  if (jwtToken.length() > 0) {
    // Verificar si el token está próximo a expirar (simplificado)
    static unsigned long lastTokenCheck = 0;
    if (millis() - lastTokenCheck > 300000) { // 5 minutos
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
    // Usar HTTPClient para WiFi
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
    // Usar EthernetClient para Ethernet
    logger("API", "Usando EthernetClient para PATCH controller");
    EthernetClient localClient;
    if (!localClient.connect(serverIp, serverPort)) {
      logger("API", "Error: No se pudo conectar al servidor via Ethernet");
      return false;
    }
    
    // Enviar petición HTTP cruda
    localClient.print("PATCH /api/v1.0/controllers/" + String(controllerId) + " HTTP/1.1\r\n");
    localClient.print("Host: 164.92.95.176\r\n");
    localClient.print("Content-Type: application/json\r\n");
    localClient.print("Authorization: Bearer " + jwtToken + "\r\n");
    localClient.print("Content-Length: ");
    localClient.println(patchPayload.length());
    localClient.print("Connection: close\r\n\r\n");
    localClient.print(patchPayload);

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
  
  // Reset del chip Ethernet (como en el test exitoso)
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(200);
  updateRelay(); // Actualizar relay durante reset
  digitalWrite(PIN_RST, HIGH);
  delay(200);
  updateRelay(); // Actualizar relay después de reset
  pinMode(PIN_INT, INPUT);
  
  // Inicializar SPI y Ethernet
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);
  
  logger("NET", "Inicializando Ethernet.begin()...");
  unsigned long startTime = millis();
  
  // ⚠️ CRÍTICO: Actualizar relay ANTES de Ethernet.begin() que puede tardar mucho
  updateRelay();
  logger("NET", "Relay antes de Ethernet.begin(): %s", RELAY_STATE ? "ON" : "OFF");
  
  // Estrategia: Intentos múltiples con timeouts cortos para actualizar relay entre intentos
  int result = 0;
  int maxAttempts = 3;
  int timeoutPerAttempt = 4000; // 4 segundos por intento
  
  for (int attempt = 1; attempt <= maxAttempts && result == 0; attempt++) {
    logger("NET", "Intento %d/%d de Ethernet.begin() (timeout: %d ms)...", attempt, maxAttempts, timeoutPerAttempt);
    
    result = Ethernet.begin(mac, timeoutPerAttempt);
    
    // ⚠️ CRÍTICO: Actualizar relay INMEDIATAMENTE después de cada intento
    updateRelay();
    
    if (result == 0 && attempt < maxAttempts) {
      logger("NET", "Intento %d falló, esperando antes de reintentar...", attempt);
      delay(500);
      updateRelay();
    }
  }
  
  unsigned long elapsed = millis() - startTime;
  logger("NET", "Ethernet.begin() completado en %lu ms, resultado: %d", elapsed, result);
  logger("NET", "Relay después de Ethernet.begin(): %s", RELAY_STATE ? "ON" : "OFF");
  
  if (result == 0) {
    logger("NET", "❌ Ethernet.begin() falló después de %d intentos - result = 0", maxAttempts);
    connectedEthernet = false;
    deviceIP = "0.0.0.0";
    updateRelay(); // Actualizar antes de retornar
    return;
  }
  
  // Verificar IP válida
  IPAddress ip = Ethernet.localIP();
  if (ip == INADDR_NONE) {
    logger("NET", "❌ IP inválida después de Ethernet.begin()");
    connectedEthernet = false;
    deviceIP = "0.0.0.0";
    updateRelay(); // Actualizar antes de retornar
    return;
  }
  
  deviceIP = ip.toString();
  connectedEthernet = true;
  CONNECTION_TYPE = "Cable";
  CURRENT_CONNECTION_STATE = true;
  logger("NET", "✅ Ethernet conectado - IP: %s", deviceIP.c_str());
  
  // Actualizar relay una última vez
  updateRelay();
}

bool tryConnectSavedWiFi(unsigned long timeoutMs = 5000) {
  logger("NET", "Intentando WiFi con credenciales guardadas...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // usar credenciales persistentes
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
    // ⚠️ IMPORTANTE: Actualizar relay cada 200ms durante espera de WiFi
    updateRelay();
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    deviceIP = WiFi.localIP().toString();
    connectedWiFi = true;
    CONNECTION_TYPE = "WiFi";
    CURRENT_CONNECTION_STATE = true;
    logger("NET", "WiFi conectado - IP: %s", deviceIP.c_str());
    updateRelay(); // Actualizar antes de retornar
    return true;
  }
  
  connectedWiFi = false;
  logger("NET", "WiFi timeout después de %lu ms", timeoutMs);
  updateRelay(); // Actualizar antes de retornar
  return false;
}

void connectWiFi(const char* ssid, const char* password, unsigned long timeoutMs = 5000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  logger("NET", "Conectando a %s...", ssid);
  
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
    // ⚠️ IMPORTANTE: Actualizar relay cada 100ms durante espera de WiFi
    updateRelay();
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    deviceIP = WiFi.localIP().toString();
    connectedWiFi = true;
    CONNECTION_TYPE = "WiFi";
    CURRENT_CONNECTION_STATE = true;
    logger("NET", "WiFi conectado - IP: %s", deviceIP.c_str());
    updateRelay(); // Actualizar antes de retornar
  } else {
    logger("NET", "No se pudo conectar a WiFi");
    connectedWiFi = false;
    updateRelay(); // Actualizar antes de retornar
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
        while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
          delay(100);
          // ⚠️ IMPORTANTE: Actualizar relay durante conexión WiFi por Bluetooth
          updateRelay();
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
    CONNECTION_TYPE = "Sin Red";
  }
}

void updateTankStatus() {
  // Tanque lleno cuando pin 25 está en LOW - ACTUALIZACIÓN EN TIEMPO REAL
  bool pin25State = digitalRead(CONTROL_PIN_2);
  bool previousTankState = TANK_FULL;
  
  // Log cada 5 segundos para debug
  static unsigned long lastTankLog = 0;
  if (millis() - lastTankLog >= 5000) {
    logWithTimestamp("TANK", "Pin 25: %s, Tanque: %s", pin25State ? "HIGH" : "LOW", TANK_FULL ? "LLENO" : "VACIO");
    lastTankLog = millis();
  }
  
  if (!pin25State) { // Pin 25 en LOW = conectado a GND = tanque lleno
    TANK_FULL = false;
    if (previousTankState != TANK_FULL) {
      logWithTimestamp("TANK", "🟢 CAMBIO: Tanque LLENO - Pin 25 conectado a GND (LOW)");
    }
  } else { // Pin 25 en HIGH = no conectado = tanque vacío
    TANK_FULL = true;
    if (previousTankState != TANK_FULL) {
      logWithTimestamp("TANK", "🔴 CAMBIO: Tanque VACÍO - Pin 25 no conectado (HIGH)");
    }
  }
}

void updateTankIcon() {
  // Actualizar solo el icono del tanque sin redibujar toda la pantalla
  int screenWidth = tft.width();
  int iconWidth = 60;
  int totalIconWidth = iconWidth * 4;
  int spacing = (screenWidth - totalIconWidth) / 5;
  
  if (spacing < 5) {
    spacing = 5;
    iconWidth = (screenWidth - spacing * 5) / 4;
  }
  
  int iconY = 10;
  int tankX = spacing + iconWidth + spacing; // Posición del icono del tanque
  
  // Limpiar área del icono del tanque
  clearArea(tankX, iconY, iconWidth, 60);
  
  // Redibujar solo el icono del tanque
  drawTankIcon(tankX, iconY, TANK_FULL);
  
  logWithTimestamp("TANK", "Icono actualizado: %s", TANK_FULL ? "LLENO" : "VACIO");
}

void updatePumpIcon() {
  // Actualizar solo el icono de la bomba sin redibujar toda la pantalla
  int screenWidth = tft.width();
  int iconWidth = 60;
  int totalIconWidth = iconWidth * 4;
  int spacing = (screenWidth - totalIconWidth) / 5;
  
  if (spacing < 5) {
    spacing = 5;
    iconWidth = (screenWidth - spacing * 5) / 4;
  }
  
  int iconY = 10;
  int pumpX = spacing + iconWidth + spacing + iconWidth + spacing; // Posición del icono de la bomba
  
  // Limpiar área del icono de la bomba (incluyendo área de texto FLUSH)
  clearArea(pumpX, iconY, iconWidth, 95);
  
  // Redibujar solo el icono de la bomba
  drawPumpIcon(pumpX, iconY, RELAY_STATE);
  
  logWithTimestamp("PUMP", "Icono actualizado: %s", RELAY_STATE ? "ACTIVA" : "INACTIVA");
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
  
  // ✅ Mostrar "FLUSH" cuando el relay de flush está activo
  if (RELAY_FLUSH_STATE || isFlushActive) {
    // Limpiar área debajo del icono para el texto FLUSH
    tft.fillRect(x, y + 75, 60, 15, TFT_WHITE);
    
    // Mostrar "FLUSH" en texto pequeño y color rojo
    tft.setTextSize(1);
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.setCursor(x + 10, y + 80);
    tft.print("FLUSH");
  } else {
    // Limpiar el área si no está en flush
    tft.fillRect(x, y + 75, 60, 15, TFT_WHITE);
  }
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
      // Icono WiFi
      tft.drawCircle(x + radius, y + radius + 5, 6, TFT_BLACK);
      tft.drawCircle(x + radius, y + radius + 5, 10, TFT_BLACK);
      tft.drawCircle(x + radius, y + radius + 5, 14, TFT_BLACK);
      tft.fillCircle(x + radius, y + radius + 5, 2, TFT_BLACK);
    } else if (type == "Cable") {
      // Icono Ethernet
      tft.fillRect(x + radius - 8, y + radius - 3, 16, 6, TFT_BLACK);
      tft.drawLine(x + radius - 10, y + radius, x + radius - 8, y + radius, TFT_BLACK);
      tft.drawLine(x + radius + 8, y + radius, x + radius + 10, y + radius, TFT_BLACK);
    }
  } else {
    // Icono sin conexión
    tft.drawLine(x + radius - 6, y + radius - 6, x + radius + 6, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius + 6, y + radius - 6, x + radius - 6, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius - 5, y + radius - 6, x + radius + 7, y + radius + 6, TFT_BLACK);
    tft.drawLine(x + radius + 5, y + radius - 6, x + radius - 7, y + radius + 6, TFT_BLACK);
  }
  
  // Dibujar etiqueta principal
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 12, y + 60);
  tft.print(label);
  
  // Si está conectado por cable, mostrar IP debajo
  if (online && type == "Cable" && deviceIP != "0.0.0.0") {
    // Limpiar área debajo del icono para la IP
    tft.fillRect(x, y + 75, 60, 15, TFT_WHITE);
    
    // Mostrar IP en texto más pequeño
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
  int y = 100;  // Entre iconos (línea 80) y tabla de lecturas (línea 130)
  int w = 300;
  int h = 25;
  
  // Limpiar área del mensaje
  clearArea(x, y - 5, w, h + 10);
  
  tft.setTextSize(2);
  tft.setTextColor(color, TFT_WHITE);
  tft.setCursor(x, y);
  tft.print(message);
  
  logWithTimestamp("STATUS", "Mensaje: %s", message.c_str());
}

void clearStatusMessage() {
  int x = 10;
  int y = 100;  // Misma posición que drawStatusMessage
  int w = 300;
  int h = 25;
  
  clearArea(x, y - 5, w, h + 10);
}

void drawBluetoothIcon(int x, int y) {
  int radius = 15;
  uint16_t color = TFT_BLUE;
  
  // Dibujar icono de Bluetooth
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  // Dibujar símbolo de Bluetooth
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
  // Actualizar solo el icono de red durante el proceso de conexión
  int screenWidth = tft.width();
  int iconWidth = 60;
  int totalIconWidth = iconWidth * 4;
  int spacing = (screenWidth - totalIconWidth) / 5;
  
  if (spacing < 5) {
    spacing = 5;
    iconWidth = (screenWidth - spacing * 5) / 4;
  }
  
  int iconY = 10;
  int networkX = spacing + (iconWidth + spacing) * 3; // Posición del icono de red
  
  // Limpiar área del icono de red
  clearArea(networkX, iconY, iconWidth, 60);
  
  if (connectionType == "Cable") {
    // Mostrar icono de cable con indicador de carga
    drawNetworkIconWithLoading(networkX, iconY, "Cable");
  } else if (connectionType == "WiFi") {
    // Mostrar icono de WiFi con indicador de carga
    drawNetworkIconWithLoading(networkX, iconY, "WiFi");
  } else if (connectionType == "Bluetooth") {
    // Mostrar icono de Bluetooth
    drawBluetoothIcon(networkX + 15, iconY + 15);
  }
  
  logWithTimestamp("NET", "Icono actualizado: %s", connectionType.c_str());
}

void drawNetworkIconWithLoading(int x, int y, String type) {
  int radius = 25;
  int iconWidth = 60; // Definir iconWidth localmente
  uint16_t color;
  String label;
  
  if (type == "WiFi") {
    color = 0x07E0;
    label = "WiFi";
  } else {
    color = 0x07FF;
    label = "Cable";
  }
  
  // Dibujar círculo base
  tft.fillCircle(x + radius, y + radius, radius, color);
  tft.drawCircle(x + radius, y + radius, radius, TFT_BLACK);
  
  // Dibujar icono según tipo
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
  
  // Dibujar indicador de carga (puntos animados)
  static unsigned long lastLoadingUpdate = 0;
  static int loadingDots = 0;
  
  if (millis() - lastLoadingUpdate > 500) {
    loadingDots = (loadingDots + 1) % 4;
    lastLoadingUpdate = millis();
  }
  
  // Limpiar área de texto
  tft.fillRect(x + 5, y + 50, iconWidth - 10, 15, TFT_WHITE);
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(x + 12, y + 60);
  tft.print(label);
  
  // Dibujar puntos de carga
  for (int i = 0; i < loadingDots; i++) {
    tft.fillCircle(x + radius - 6 + i * 4, y + radius + 20, 2, TFT_BLACK);
  }
}

void drawCompleteDisplay() {
  logWithTimestamp("DISPLAY", "🔄 Redibujando pantalla completa...");
  
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
  
  // Dibujar iconos
  drawPowerIcon(currentX, iconY, POWER_ON);
  currentX += iconWidth + spacing;
  
  drawTankIcon(currentX, iconY, TANK_FULL);
  currentX += iconWidth + spacing;
  
  drawPumpIcon(currentX, iconY, RELAY_STATE);
  currentX += iconWidth + spacing;
  
  drawNetworkIcon(currentX, iconY, CURRENT_CONNECTION_STATE, CONNECTION_TYPE);
  
  // Dibujar etiquetas estáticas
  int startY = 130;
  int lineH = 35;
  int avgX = screenWidth * 0.80;
  
  tft.setTextSize(2);
  tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
  tft.setCursor(10, startY);
  tft.print("Produccion:");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.setCursor(avgX, startY + 8);
  tft.printf("/ %.0fL", AVG_FLOW_PROD);
  
  tft.setTextColor(0xF800, TFT_WHITE);
  tft.setCursor(10, startY + lineH);
  tft.print("Rechazo:");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.setCursor(avgX, startY + lineH + 8);
  tft.printf("/ %.0fL", AVG_FLOW_RECH);
  
  tft.setTextColor(0xFD20, TFT_WHITE);
  tft.setCursor(10, startY + lineH * 2);
  tft.print("TDS:");
  tft.setTextColor(TFT_DARKGREY, TFT_WHITE);
  tft.setCursor(avgX, startY + lineH * 2 + 8);
  tft.printf("/ %.0f", AVG_TDS);
  
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
  lastTankState = TANK_FULL;
  lastRelayState = RELAY_STATE;
  lastConnectionState = CURRENT_CONNECTION_STATE;
  lastConnectionType = CONNECTION_TYPE;
  lastProductName = product_name;
  
  logWithTimestamp("DISPLAY", "✅ Pantalla completa redibujada");
}

void updateDisplayValues() {
  int startY = 130;
  int lineH = 35;
  int screenWidth = tft.width();
  int valueX = screenWidth * 0.45;
  int avgX = screenWidth * 0.80;
  
  // SIEMPRE actualizar valores - sin restricciones de cambio
  // Actualizar valor de producción
  clearArea(valueX, startY, avgX - valueX - 10, 30);
  tft.setTextSize(2);
  tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
  tft.setCursor(valueX, startY);
  tft.printf("%.2f L/m", CURRENT_FLOW_PROD);
  lastFlowProd = CURRENT_FLOW_PROD;
  
  // Actualizar valor de rechazo
  clearArea(valueX, startY + lineH, avgX - valueX - 10, 30);
  tft.setTextSize(2);
  tft.setTextColor(0xF800, TFT_WHITE);
  tft.setCursor(valueX, startY + lineH);
  tft.printf("%.2f L/m", CURRENT_FLOW_RECH);
  lastFlowRech = CURRENT_FLOW_RECH;
  
  // Actualizar valor de TDS
  clearArea(valueX, startY + lineH * 2, avgX - valueX - 10, 30);
  tft.setTextSize(2);
  tft.setTextColor(0xFD20, TFT_WHITE);
  tft.setCursor(valueX, startY + lineH * 2);
  tft.printf("%.0f ppm", CURRENT_TDS);
  lastTDS = CURRENT_TDS;
  
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
  
  // Actualizar animación de bomba
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
    
    clearArea(pumpX, 10, iconWidth, 95);
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
      logWithTimestamp("TIMER", "Contador actualizado: %lu seg (desde último updateController)", displayTime);
    }
    
    lastTimerUpdate = now;
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  SerialBT.begin("AquaTech_Controller");
  logger("INFO", "=== INICIANDO AQUATECH CONTROLLER LIMPIO ===");
  
  // Configurar pines
  pinMode(TDS_PIN, INPUT);
  pinMode(FLOW_PIN_PROD, INPUT_PULLUP);
  pinMode(FLOW_PIN_RECH, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(RELAY_FLUSH_PIN, OUTPUT);  // ✅ Pin para relay de flush
  pinMode(CONTROL_PIN_1, INPUT_PULLUP);
  pinMode(CONTROL_PIN_2, INPUT_PULLUP);
  pinMode(FLUSH_BUTTON_PIN, INPUT_PULLUP);  // ✅ Botón de flush manual
  
  // Configurar ADC para TDS
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);
  
  // Interrupciones para sensores de flujo
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_PROD), flowInterruptProd, RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_RECH), flowInterruptRech, RISING);
  
  // Estado inicial del relay
  // ✅ Estado inicial de los relays - AMBOS usan lógica NORMAL (HIGH=ON, LOW=OFF)
  relayState = false;
  digitalWrite(RELAY_PIN, LOW);  // ✅ Relay 1 OFF (lógica normal: LOW=OFF)
  RELAY_STATE = relayState;
  logger("RELAY", "Estado inicial Relay Principal (Pin 16): OFF (LOW)");
  
  // Relay flush OFF inicialmente
  digitalWrite(RELAY_FLUSH_PIN, LOW);  // ✅ Relay 2 OFF (lógica normal: LOW=OFF)
  RELAY_FLUSH_STATE = false;
  isFlushActive = false;
  logger("FLUSH", "Estado inicial Relay Flush (Pin 17): OFF (LOW)");
  
  // Verificar estado físico de los pines
  delay(100);
  int pin16State = digitalRead(RELAY_PIN);
  int pin17State = digitalRead(RELAY_FLUSH_PIN);
  int pin12State = digitalRead(FLUSH_BUTTON_PIN);
  logger("RELAY", "✓ Verificación Pin 16: %s (esperado: LOW=OFF)", pin16State == LOW ? "LOW" : "HIGH");
  logger("FLUSH", "✓ Verificación Pin 17: %s (esperado: LOW=OFF)", pin17State == LOW ? "LOW" : "HIGH");
  logger("FLUSH", "✓ Botón Flush Manual (Pin 12): %s (LOW=activar flush)", pin12State == LOW ? "LOW (ACTIVO)" : "HIGH (inactivo)");
  
  // Inicializar lastFlushTime
  lastFlushTime = millis();
  logger("FLUSH", "⏰ Flush programado: cada %lu segundos", FLUSH_INTERVAL / 1000);
  logger("FLUSH", "✅ Botón de flush manual configurado en Pin 12 (LOW = activar)");
  
  // Verificar estado inicial del pin 25 (tanque)
  bool initialPin25State = digitalRead(CONTROL_PIN_2);
  logger("TANK", "Estado inicial Pin 25: %s", initialPin25State ? "HIGH (desconectado)" : "LOW (conectado)");
  
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
  tft.writedata(0x28);
  
  tft.fillScreen(TFT_WHITE);
  
  // Inicializar conectividad de red (ya muestra la tabla internamente)
  initializeNetwork();
  
  // Obtener configuración del controller si hay conexión
  if (connectedEthernet || connectedWiFi) {
    logger("SETUP", "Obteniendo configuración inicial del controller...");
    getControllerById();
  } else {
    logger("SETUP", "Sin conexión - usando valores por defecto");
  }
  
  logger("INFO", "=== SETUP COMPLETADO - VERSIÓN LIMPIA ===");
}

// ================== MAIN LOOP ==================
void loop() {
  unsigned long now = millis();
  
  // ====== ACTUALIZACIÓN DEL RELAY DE FLUSH (PRIORIDAD MÁXIMA) ======
  updateFlushRelay();
  
  // ====== LECTURA CONTINUA DE SENSORES ======
  readTDSSensor();
  updateFlowRates();
  updateSystemLogic();
  
  // ====== ACTUALIZACIÓN EN TIEMPO REAL DEL TANQUE ======
  updateTankStatus();
  
  // ====== ACTUALIZAR ICONO DEL TANQUE EN TIEMPO REAL ======
  static bool lastTankState = false;
  if (TANK_FULL != lastTankState) {
    updateTankIcon();
    lastTankState = TANK_FULL;
  }
  
  // ====== ACTUALIZAR ICONO DE LA BOMBA EN TIEMPO REAL ======
  static bool lastRelayState = false;
  if (RELAY_STATE != lastRelayState) {
    updatePumpIcon();
    lastRelayState = RELAY_STATE;
  }
  
  // ====== FLUJO 1: CONTADOR DE TIEMPO INDEPENDIENTE ======
  updateDisplayTimer();
  
  // ====== FLUJO 2: ACTUALIZACIÓN DE PANTALLA Y ENVÍO DE DATOS ======
  if (now - lastLoop >= loopTime) {
    logWithTimestamp("LOOP", "🔄 ACTUALIZANDO VALORES (loopTime: %lu ms)", loopTime);
    updateDisplayValues();
    
    // Enviar datos si hay conexión
    if (connectedEthernet || connectedWiFi) {
      enviarDatos(productId.c_str(), CURRENT_FLOW_PROD, CURRENT_FLOW_RECH, CURRENT_TDS, 25.0);
    }
    
    lastLoop = now;
    logWithTimestamp("LOOP", "✅ ACTUALIZACIÓN DE VALORES COMPLETADA");
  }
  
  // ====== ACTUALIZACIÓN ADICIONAL DE VALORES CADA 500ms ======
  static unsigned long lastQuickUpdate = 0;
  if (now - lastQuickUpdate >= 500) {
    updateDisplayValues();
    lastQuickUpdate = now;
  }
  
  // ====== FLUJO 4: CICLO COMPLETO updateControllerTime ======
  if (now - lastUpdateController >= updateControllerTime) {
    logWithTimestamp("CYCLE", "🔄 INICIANDO CICLO COMPLETO updateControllerTime (%lu ms)", updateControllerTime);
    
    // Actualizar configuración del controller si hay conexión
    if (connectedEthernet || connectedWiFi) {
      logWithTimestamp("API", "Obteniendo configuración actualizada del controller...");
      getControllerById();
      
      // Verificar si se solicita reinicio remoto
      if (reset_pending) {
        logWithTimestamp("RESET", "⚠️ REINICIO REMOTO DETECTADO - Ejecutando...");
        patchControllerResetPending();
        delay(1000);
        ESP.restart();
      }
    }
    
    // Redibujar display completo
    logWithTimestamp("DISPLAY", "Redibujando pantalla completa...");
    drawCompleteDisplay();
    lastUpdateController = now;
    
    if (connectedEthernet || connectedWiFi) {
      logWithTimestamp("CYCLE", "✅ CICLO COMPLETO FINALIZADO - Con red");
    } else {
      logWithTimestamp("CYCLE", "✅ CICLO COMPLETO FINALIZADO - Sin red");
    }
  }
  
  // ====== MANEJO DE BLUETOOTH (solo si no hay red) ======
  if (!connectedEthernet && !connectedWiFi) {
    handleBluetoothWiFi();
  }
  
  // Delay mínimo
  delay(10);
}
