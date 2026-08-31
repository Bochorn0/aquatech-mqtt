// ================== AQUATECH EQUYSIS FLOW — COMPLETO ==================
// Cableado medidor:
//   Verde OUTPUT → GPIO 19
//   Azul  GND    → GND
//
// WiFi por Bluetooth (mismo protocolo que sensor_presion_v2):
//   1. Emparejar SPP "AquaTech_Equysis" (solo si NO hay WiFi)
//   2. Enviar: NOMBRE_RED,PASSWORD_RED
//   3. Para borrar: CLEAR_WIFI
// Con WiFi conectado el BT se apaga: TLS/HTTPS necesita ~70 KB de heap.
//
// Serial Monitor 115200. Emite JSON a aquatech_api cada 10 minutos.

#include "BluetoothSerial.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_bt.h>
#include <time.h>

BluetoothSerial SerialBT;
bool bluetoothStarted = false;
const char* btName = "AquaTech_Equysis";

// ================== TIMING ==================
unsigned long updateControllerTime = 60000;
unsigned long loopTime = 10UL * 60UL * 1000UL;  // POST cada 10 minutos
unsigned long lastUpdateController = 0;
unsigned long lastLoop = 0;
unsigned long lastWifiRetry = 0;

bool devMode = true;
#define USE_MOCK_DATA false  // true = pulsos inventados; false = GPIO 19
#define USE_CONTROLLER false  // Equysis no tiene fila en controllers (prod usa bigint)

// ================== EQUYSIS ==================
const int PIN_METER_OUTPUT = 19;
const uint32_t BOOT_IGNORE_MS = 1500;
const uint32_t PULSE_MIN_US = 2000;
const uint32_t PULSE_MAX_US = 15000;

// Lectura del LCD al cargar. Ajusta al valor actual del medidor.
// 1 pulso ≈ 0.0001 m³ (0.1 L). Caudal = litros del intervalo / minutos.
double m3Inicial = 0.00811;
double m3PorPulso = 0.0001;

volatile uint32_t pulseCount = 0;
volatile uint32_t rejectedCount = 0;
volatile uint32_t lastLowUs = 0;
volatile uint32_t lastHighUs = 0;
volatile uint32_t fallUs = 0;
volatile uint32_t minLowUs = 0xFFFFFFFF;
volatile uint32_t maxLowUs = 0;
volatile int irqLevel = HIGH;

uint32_t lastCount = 0;

double m3Medidor = 0;
double litrosMedidor = 0;
double litrosPorMin = 0;
double m3Ventana = 0;
uint32_t pulsosIntervalo = 0;
uint32_t pulsosTotal = 0;

// ================== NETWORK / API ==================
// Extraido de sensor_presion_v2.ino — cambia productId/controllerId de este equipo.
bool connectedWiFi = false;
String deviceIP = "0.0.0.0";
String jwtToken = "";
String productId = "manual-1787240813479";
const bool LOCK_PRODUCT_ID = true;
String product_name = "AquaTech Equysis Flow";
String CONNECTION_TYPE = "Sin Red";
bool CURRENT_CONNECTION_STATE = false;
bool reset_pending = false;
const char* controllerId = "692c9f9caed050879fb8426c";

const char* apiHost = "lccapp-aefcbwh0ecd7b8cz.canadacentral-01.azurewebsites.net";
const char* loginPath = "/api/v1.0/auth/login";
const char* controllerPath = "/api/v1.0/controllers/692c9f9caed050879fb8426c";
const char* dataPath = "/api/v1.0/products/componentInput";
const char* loginEmail = "esp32@lcc.com.mx";
const char* loginPassword = "CHANGE_ME";
#define USE_STATIC_TOKEN false
const char* staticJwt = "";
const uint32_t HTTP_TIMEOUT_MS = 20000;

WiFiClientSecure apiClient;

// ================== DECLARATIONS ==================
void logger(const char* titulo, const char* formato, ...);
bool tryConnectSavedWiFi(unsigned long timeoutMs);
void handleBluetoothWiFi();
void updateNetworkStatus();
void obtenerToken();
String getControllerById();
void enviarDatos();
bool patchControllerResetPending();
void snapshotMeter(unsigned long elapsedMs);
void applyMockReading(unsigned long elapsedMs);
void logReading();
void logLine(const char* titulo, const String& s);
String clipForLog(const String& s, unsigned maxLen = 1500);
int httpsCall(const char* method, const char* path, const String& body, const String& bearer, String& responseBody);
void syncNtp();
void startBluetooth();
void stopBluetooth();

// ================== UTILS ==================
void logger(const char* titulo, const char* formato, ...) {
  char buffer[256];
  va_list args;
  va_start(args, formato);
  vsnprintf(buffer, sizeof(buffer), formato, args);
  va_end(args);
  Serial.printf("[%s] %s\n", titulo, buffer);
  if (bluetoothStarted) {
    SerialBT.printf("[%s] %s\n", titulo, buffer);
  }
}

void logLine(const char* titulo, const String& s) {
  Serial.printf("[%s] %s\n", titulo, s.c_str());
  if (!bluetoothStarted) return;
  SerialBT.printf("[%s] ", titulo);
  const unsigned chunk = 180;
  for (unsigned i = 0; i < s.length(); i += chunk) {
    SerialBT.print(s.substring(i, i + chunk));
  }
  SerialBT.println();
}

String clipForLog(const String& s, unsigned maxLen) {
  if (s.length() <= maxLen) return s;
  return s.substring(0, maxLen) + "...";
}

void startBluetooth() {
  if (bluetoothStarted) return;
  if (!SerialBT.begin(btName)) {
    Serial.printf("[BT] No se pudo iniciar SPP\n");
    return;
  }
  bluetoothStarted = true;
  logger("BT", "encendido %s heap=%u — envia SSID,PASSWORD o CLEAR_WIFI",
         btName, ESP.getFreeHeap());
}

void stopBluetooth() {
  if (!bluetoothStarted) return;
  SerialBT.flush();
  SerialBT.end();
  bluetoothStarted = false;
  delay(50);
  logger("BT", "apagado heap=%u", ESP.getFreeHeap());
}

int httpsCall(const char* method, const char* path, const String& body, const String& bearer, String& responseBody) {
  responseBody = "";
  apiClient.stop();
  apiClient.setInsecure();
  apiClient.setHandshakeTimeout(30);
  apiClient.setTimeout(HTTP_TIMEOUT_MS);

  String url = String("https://") + apiHost + path;
  logger("API", ">>> %s %s", method, url.c_str());
  logger("API", ">>> Content-Length=%u Auth=%s",
         body.length(), bearer.length() > 0 ? "Bearer" : "no");
  if (body.length() > 0) {
    String bodyToLog = body;
    if (strcmp(path, loginPath) == 0) {
      bodyToLog = String("{\"email\":\"") + loginEmail + "\",\"password\":\"***\"}";
    }
    logLine("API", String(">>> body ") + bodyToLog);
  } else {
    logger("API", ">>> body (vacio)");
  }

  unsigned long t0 = millis();
  logger("API", "heap=%u BT=%s TLS %s:443",
         ESP.getFreeHeap(), bluetoothStarted ? "on" : "off", apiHost);
  if (!apiClient.connect(apiHost, 443)) {
    char err[80] = {0};
    apiClient.lastError(err, sizeof(err));
    logger("API", "<<< TLS fail %s elapsed=%lu ms", err[0] ? err : "connect()", millis() - t0);
    return -1;
  }
  logger("API", "TLS OK peer=%s handshake=%lu ms",
         apiClient.remoteIP().toString().c_str(), millis() - t0);

  String req = String(method) + " " + path + " HTTP/1.1\r\n";
  req += String("Host: ") + apiHost + "\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Connection: close\r\n";
  if (bearer.length() > 0) {
    req += "Authorization: Bearer " + bearer + "\r\n";
  }
  req += "Content-Length: " + String(body.length()) + "\r\n\r\n";
  req += body;
  apiClient.print(req);
  logger("API", ">>> enviado %u bytes HTTP", req.length());

  unsigned long start = millis();
  String raw;
  while (millis() - start < HTTP_TIMEOUT_MS) {
    while (apiClient.available()) {
      raw += (char)apiClient.read();
      if (raw.length() > 6000) break;
    }
    if (!apiClient.connected() && !apiClient.available()) break;
    delay(5);
  }
  apiClient.stop();

  int code = -1;
  int s1 = raw.indexOf(' ');
  int s2 = raw.indexOf(' ', s1 + 1);
  if (s1 > 0 && s2 > s1) {
    code = raw.substring(s1 + 1, s2).toInt();
  }
  int bodyPos = raw.indexOf("\r\n\r\n");
  if (bodyPos >= 0) {
    responseBody = raw.substring(bodyPos + 4);
  }

  logger("API", "<<< HTTP %d raw=%u body=%u elapsed=%lu ms",
         code, raw.length(), responseBody.length(), millis() - t0);
  if (raw.length() == 0) {
    logger("API", "<<< sin respuesta del servidor");
  } else {
    int eol = raw.indexOf("\r\n");
    if (eol > 0) {
      logLine("API", String("<<< status ") + raw.substring(0, eol));
    }
    if (responseBody.length() > 0) {
      logLine("API", String("<<< body ") + clipForLog(responseBody));
    } else {
      logger("API", "<<< body vacio");
    }
  }
  return code;
}

void syncNtp() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 20 && time(nullptr) < 1700000000; i++) {
    delay(250);
  }
  IPAddress resolved;
  if (WiFi.hostByName(apiHost, resolved)) {
    logger("NET", "DNS %s -> %s", apiHost, resolved.toString().c_str());
  } else {
    logger("NET", "DNS fail %s", apiHost);
  }
}

// ================== PULSOS EQUYSIS ==================
void IRAM_ATTR onEdge() {
  const uint32_t now = micros();
  const int level = digitalRead(PIN_METER_OUTPUT);

  if (irqLevel == HIGH && level == LOW) {
    lastHighUs = now - fallUs;
    fallUs = now;
  } else if (irqLevel == LOW && level == HIGH) {
    const uint32_t width = now - fallUs;
    lastLowUs = width;
    if (width >= PULSE_MIN_US && width <= PULSE_MAX_US) {
      pulseCount++;
      if (width < minLowUs) minLowUs = width;
      if (width > maxLowUs) maxLowUs = width;
    } else {
      rejectedCount++;
    }
  }
  irqLevel = level;
}

void applyMockReading(unsigned long elapsedMs) {
  const uint32_t extra = 2 + (uint32_t)(millis() / 1000 % 3);
  pulsosIntervalo = extra;
  pulsosTotal += extra;
  pulseCount = pulsosTotal;
  lastCount = pulsosTotal;

  m3Ventana = (double)pulsosIntervalo * m3PorPulso;
  m3Medidor = m3Inicial + (double)pulsosTotal * m3PorPulso;
  litrosMedidor = m3Medidor * 1000.0;
  const double litrosVentana = m3Ventana * 1000.0;
  if (elapsedMs > 0) {
    litrosPorMin = litrosVentana * (60000.0 / (double)elapsedMs);
  } else {
    litrosPorMin = 2.4;
  }
}

void snapshotMeter(unsigned long elapsedMs) {
#if USE_MOCK_DATA
  applyMockReading(elapsedMs);
  return;
#endif

  noInterrupts();
  const uint32_t count = pulseCount;
  const uint32_t rejected = rejectedCount;
  const uint32_t lowUs = lastLowUs;
  interrupts();

  pulsosIntervalo = count - lastCount;
  lastCount = count;
  pulsosTotal = count;

  const double m3DesdeBoot = (double)count * m3PorPulso;
  m3Medidor = m3Inicial + m3DesdeBoot;
  m3Ventana = (double)pulsosIntervalo * m3PorPulso;
  litrosMedidor = m3Medidor * 1000.0;
  const double litrosVentana = m3Ventana * 1000.0;
  if (elapsedMs > 0) {
    litrosPorMin = litrosVentana * (60000.0 / (double)elapsedMs);
  } else {
    litrosPorMin = 0;
  }

  logger("DATA", "gpio=%s pulso_us=%lu rechazados=%lu ventana_ms=%lu",
         digitalRead(PIN_METER_OUTPUT) ? "HIGH" : "LOW",
         (unsigned long)lowUs, (unsigned long)rejected, elapsedMs);
}

void logReading() {
  logger("DATA", "%sm3=%.5f L=%.5f L/min=%.5f pulsos=%lu delta=%lu",
#if USE_MOCK_DATA
         "MOCK ",
#else
         "",
#endif
         m3Medidor, litrosMedidor, litrosPorMin,
         (unsigned long)pulsosTotal, (unsigned long)pulsosIntervalo);
}

// ================== WIFI ==================
bool tryConnectSavedWiFi(unsigned long timeoutMs = 10000) {
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
    stopBluetooth();
    logger("NET", "WiFi OK - IP: %s heap=%u", deviceIP.c_str(), ESP.getFreeHeap());
    return true;
  }

  connectedWiFi = false;
  CURRENT_CONNECTION_STATE = false;
  CONNECTION_TYPE = "Sin Red";
  deviceIP = "0.0.0.0";
  logger("NET", "Sin WiFi. Envia por BT: SSID,PASSWORD");
  return false;
}

void handleBluetoothWiFi() {
  if (!bluetoothStarted) return;
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
        logger("BT", "Desconectando WiFi y reiniciando...");
        WiFi.disconnect(true, true);
        delay(500);
        ESP.restart();
      }

      if (!connectedWiFi) {
        int comaIndex = credenciales.indexOf(',');
        if (comaIndex > 0) {
          String ssid = credenciales.substring(0, comaIndex);
          String password = credenciales.substring(comaIndex + 1);
          ssid.trim();
          password.trim();

          logger("BT", "Conectando a SSID: %s", ssid.c_str());
          WiFi.mode(WIFI_STA);
          WiFi.persistent(true);
          WiFi.begin(ssid.c_str(), password.c_str());

          unsigned long start = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
            delay(100);
          }

          if (WiFi.status() == WL_CONNECTED) {
            deviceIP = WiFi.localIP().toString();
            connectedWiFi = true;
            CONNECTION_TYPE = "WiFi";
            CURRENT_CONNECTION_STATE = true;
            logger("BT", "WiFi OK IP: %s — reiniciando", deviceIP.c_str());
            delay(300);
            ESP.restart();
          } else {
            WiFi.persistent(false);
            logger("BT", "No se pudo conectar a %s", ssid.c_str());
          }
        } else {
          logger("BT", "Formato invalido. Usa: NOMBRE_RED,PASSWORD_RED");
        }
      } else {
        logger("BT", "WiFi ya conectado. Usa CLEAR_WIFI");
      }
      credenciales = "";
    } else {
      credenciales += c;
      if (credenciales.length() > 200) credenciales = credenciales.substring(0, 200);
    }
  }
}

void updateNetworkStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    connectedWiFi = true;
    CURRENT_CONNECTION_STATE = true;
    CONNECTION_TYPE = "WiFi";
    deviceIP = WiFi.localIP().toString();
  } else {
    connectedWiFi = false;
    CURRENT_CONNECTION_STATE = false;
    CONNECTION_TYPE = "Sin Red";
    deviceIP = "0.0.0.0";
  }
}

// ================== API ==================
void obtenerToken() {
  if (!connectedWiFi) return;

#if USE_STATIC_TOKEN
  jwtToken = staticJwt;
  logger("API", "Usando token estatico");
  return;
#endif

  logger("API", "Login email=%s", loginEmail);

  String loginPayload;
  StaticJsonDocument<256> loginDoc;
  loginDoc["email"] = loginEmail;
  loginDoc["password"] = loginPassword;
  serializeJson(loginDoc, loginPayload);

  String response;
  int httpCode = httpsCall("POST", loginPath, loginPayload, "", response);

  jwtToken = "";
  if (response.length() == 0) {
    logger("API", "Login sin respuesta, codigo %d", httpCode);
    return;
  }

  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, response);
  if (!error) {
    if (doc.containsKey("token")) {
      jwtToken = doc["token"].as<String>();
    } else if (doc.containsKey("access_token")) {
      jwtToken = doc["access_token"].as<String>();
    }
  }

  if (jwtToken == "") {
    int tokenIndex = response.indexOf("\"token\":\"");
    if (tokenIndex != -1) {
      int start = tokenIndex + 9;
      int end = response.indexOf("\"", start);
      jwtToken = response.substring(start, end);
    }
  }

  if (jwtToken != "") {
    logger("API", "Token OK len=%u", jwtToken.length());
  } else {
    logger("API", "No se encontro token, codigo %d", httpCode);
  }
}

String getControllerById() {
  if (!connectedWiFi) return "";

  if (jwtToken == "") {
    obtenerToken();
    delay(500);
    if (jwtToken == "") return "";
  }

  String json;
  int httpCode = httpsCall("GET", controllerPath, "", jwtToken, json);

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    logger("CONFIG", "Error parseando controller");
    return json;
  }

  logger("CONFIG", "controller http=%d", httpCode);

  if (doc.containsKey("update_controller_time")) {
    unsigned long v = doc["update_controller_time"].as<unsigned long>();
    if (v >= 10000 && v != updateControllerTime) {
      updateControllerTime = v;
      logger("CONFIG", "update_controller_time=%lu ms", updateControllerTime);
    }
  }

  if (doc.containsKey("loop_time")) {
    unsigned long v = doc["loop_time"].as<unsigned long>();
    if (v >= 500 && v != loopTime) {
      loopTime = v;
      logger("CONFIG", "loop_time=%lu ms", loopTime);
    }
  }

  String newProductId = "";
  if (doc.containsKey("product")) {
    newProductId = doc["product"].as<String>();
  } else if (doc.containsKey("productId")) {
    newProductId = doc["productId"].as<String>();
  }
  if (!LOCK_PRODUCT_ID && newProductId.length() > 0 && newProductId != productId) {
    logger("CONFIG", "productId %s -> %s", productId.c_str(), newProductId.c_str());
    productId = newProductId;
  }

  if (doc.containsKey("product_name")) {
    String name = doc["product_name"].as<String>();
    if (name.length() > 0) product_name = name;
  }

  if (doc.containsKey("reset_pending")) {
    reset_pending = doc["reset_pending"].as<bool>();
  }

  if (doc.containsKey("m3_por_pulso")) {
    double v = doc["m3_por_pulso"].as<double>();
    if (v > 0) m3PorPulso = v;
  }

  if (doc.containsKey("m3_inicial")) {
    m3Inicial = doc["m3_inicial"].as<double>();
  }

  if (json.indexOf("invalid") != -1 || json.indexOf("expired") != -1) {
    jwtToken = "";
  }

  return json;
}

bool patchControllerResetPending() {
  if (!connectedWiFi) return false;
  if (jwtToken == "") {
    obtenerToken();
    delay(500);
    if (jwtToken == "") return false;
  }

  String response;
  int httpCode = httpsCall("PATCH", controllerPath, "{\"reset_pending\":false}", jwtToken, response);
  return httpCode > 0;
}

void enviarDatos() {
  if (!connectedWiFi) return;

  if (jwtToken == "") {
    obtenerToken();
    delay(500);
    if (jwtToken == "") {
      logger("API", "Sin token, no se envia");
      return;
    }
  }

  String jsonPayload = "{";
  jsonPayload += "\"productId\":\"" + productId + "\",";
  jsonPayload += "\"flujo_prod\":" + String(litrosPorMin, 5) + ",";
  jsonPayload += "\"flujo_rech\":0,";
  jsonPayload += "\"tds\":0,";
  jsonPayload += "\"temperature\":25.00,";
  jsonPayload += "\"timestamp\":\"" + String(millis()) + "\",";
  jsonPayload += "\"m3\":" + String(m3Medidor, 5) + ",";
  jsonPayload += "\"litros\":" + String(litrosMedidor, 5) + ",";
  jsonPayload += "\"production_volume\":" + String(litrosMedidor, 5) + ",";
  jsonPayload += "\"pulsos_total\":" + String(pulsosTotal) + ",";
  jsonPayload += "\"pulsos_intervalo\":" + String(pulsosIntervalo) + ",";
#if USE_MOCK_DATA
  jsonPayload += "\"source\":\"equysis_mock\"";
#else
  jsonPayload += "\"source\":\"equysis\"";
#endif
  jsonPayload += "}";

  logger("API", "Enviando lecturas m3=%.5f L=%.5f L/min=%.5f pulsos=%lu",
         m3Medidor, litrosMedidor, litrosPorMin, (unsigned long)pulsosTotal);

  String response;
  int httpCode = httpsCall("POST", dataPath, jsonPayload, jwtToken, response);

  if (httpCode == 200 || httpCode == 201) {
    logger("API", "componentInput OK http=%d", httpCode);
  } else {
    logger("API", "componentInput FAIL http=%d", httpCode);
  }
  if (response.indexOf("product_log_saved") != -1) {
    logger("API", "product_log_saved presente en respuesta");
  }

  if (httpCode == 401 || response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
    logger("API", "Token invalido/expirado, se pedira de nuevo");
    jwtToken = "";
  }

  static unsigned long lastTokenCheck = 0;
  if (millis() - lastTokenCheck > 300000) {
    jwtToken = "";
    lastTokenCheck = millis();
  }
}

// ================== SETUP / LOOP ==================
void setup() {
  Serial.begin(115200);
  delay(300);

  // BLE no se usa (solo SPP classic). Liberarlo antes de WiFi/TLS.
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

  logger("SYS", "Equysis boot productId=%s mock=%s gpio=%d m3_inicial=%.5f m3/pulso=%.5f heap=%u",
         productId.c_str(), USE_MOCK_DATA ? "ON" : "OFF", PIN_METER_OUTPUT,
         m3Inicial, m3PorPulso, ESP.getFreeHeap());

#if !USE_MOCK_DATA
  pinMode(PIN_METER_OUTPUT, INPUT_PULLUP);
  delay(BOOT_IGNORE_MS);
  irqLevel = digitalRead(PIN_METER_OUTPUT);
  fallUs = micros();
  pulseCount = 0;
  rejectedCount = 0;
  lastCount = 0;
  attachInterrupt(digitalPinToInterrupt(PIN_METER_OUTPUT), onEdge, CHANGE);
#endif

  tryConnectSavedWiFi(10000);
  if (connectedWiFi) {
    logger("SYS", "BT apagado para dejar RAM a TLS heap=%u", ESP.getFreeHeap());
    syncNtp();
  } else {
    startBluetooth();
  }

#if USE_CONTROLLER
  if (devMode && connectedWiFi) {
    getControllerById();
  }
#endif

  lastLoop = 0;  // primer POST al arrancar, luego cada loopTime
  lastUpdateController = millis();
  logger("SYS", USE_MOCK_DATA ? "Listo. Enviando datos de prueba cada 10 min." : "Listo. GPIO 19 escuchando pulsos Equysis. POST cada 10 min.");
}

void loop() {
  unsigned long now = millis();

  handleBluetoothWiFi();
  updateNetworkStatus();

  if (connectedWiFi) {
    if (bluetoothStarted) stopBluetooth();
  } else if (!bluetoothStarted) {
    startBluetooth();
  }

  if (!connectedWiFi && now - lastWifiRetry >= 30000) {
    tryConnectSavedWiFi(5000);
    lastWifiRetry = now;
  }

  if (lastLoop == 0 || now - lastLoop >= loopTime) {
    const unsigned long elapsed = now - lastLoop;
    lastLoop = now;
    snapshotMeter(elapsed);
    logReading();

    if (devMode && connectedWiFi) {
      enviarDatos();
    } else if (!connectedWiFi) {
      logger("NET", "Sin WiFi. Lectura local solamente.");
    }
  }

  if (now - lastUpdateController >= updateControllerTime) {
    lastUpdateController = now;
#if USE_CONTROLLER
    if (devMode && connectedWiFi) {
      getControllerById();
      if (reset_pending) {
        logger("RESET", "Reinicio remoto");
        patchControllerResetPending();
        delay(1000);
        ESP.restart();
      }
    }
#endif
  }

  delay(10);
}
