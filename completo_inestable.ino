#include <SPI.h>
#include <Ethernet.h>
#include <TFT_eSPI.h>
#include "BluetoothSerial.h"
#include <driver/adc.h>   // Para atenuación ADC en ESP32
#include <time.h>   // ⬅️ Para NTP
#include <ArduinoJson.h>
#include <WiFi.h>

// ================== DEFAULT ==================
unsigned long lapso_actualizacion = 60000;  // default 60s
unsigned long lapso_loop = 1000;            // default 1s
bool reset_pending = false;
unsigned long lastUpdateController = 0;
bool connectedEthernet = false;
bool connectedWiFi = false;
unsigned long lastLoop = 0;  // declararlo global al inicio

// ==================  PIN RED ETHERNET ==================
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
String jwtToken = "";
const char* productId = "67d262aacf18fdaf14ec2e75";
const char* controllerId = "68cb5159a742c4cf5c4b53b1";

String deviceIP = "0.0.0.0";

// ================== CONFIGURACIÓN NTP ==================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -7 * 3600;   // Hermosillo = UTC-7
const int daylightOffset_sec = 0;       // No DST

// ================== SENSOR TDS ==================
BluetoothSerial SerialBT;
TFT_eSPI tft = TFT_eSPI();

const int TDS_PIN = 32;
const int NUM_SAMPLES = 50;   // Número de muestras para promediar
const float ALPHA = 0.1f;     // Filtro exponencial (0-1)
float calibrationFactor = 1.58f; // Ajuste TDS
float tdsFiltered = 0.0f;
float lastVoltage = 0.0f;

int tdsRaw = 0;
int tdsValue = 0;

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

void leerTDS() {
  float voltage = readAverageVoltage();
  lastVoltage = voltage;

  // Umbral mínimo de voltaje para protección
  const float V_MIN = 0.05;   // Voltios, ajustable según tu sensor
  float tds_ppm;

  if (voltage < V_MIN) {
    // Voltaje muy bajo: establecer TDS mínimo o aplicar factor de compensación
    tds_ppm = 35.0 + (voltage / V_MIN) * (calibrationFactor * voltage * 100.0);
    // Esto asegura que nunca baje de ~35 ppm
  } else {
    tds_ppm = voltage * calibrationFactor;
  }

  // Filtro exponencial
  tdsFiltered = tdsFiltered * (1 - ALPHA) + tds_ppm * ALPHA;

  tdsRaw = (int)tds_ppm;
  tdsValue = (int)tdsFiltered;

  Serial.printf("[BT] RAW=%d | Volt=%.3f V | TDScrudo=%d ppm | TDSajust=%d ppm\n",
                tdsRaw, voltage, tdsRaw, tdsValue);
  SerialBT.printf("[BT] RAW=%d | Volt=%.3f V | TDScrudo=%d ppm | TDSajust=%d ppm\n",
                tdsRaw, voltage, tdsRaw, tdsValue);
}


// ================== FLUJOS ==================
float kFactor = 55.0f;  // valor por defecto calibrado
const int FLOW_PIN_PROD = 15;
const int FLOW_PIN_RECH = 4;
volatile int flowPulseProd = 0;
volatile int flowPulseRech = 0;
float flowRateProd = 0.0;
float flowRateRech = 0.0;
float volumenProd = 0.0;
float volumenRech = 0.0;
unsigned long lastFlowCalc = 0;

// ================== RELAY ==================
const int RELAY_PIN = 19;
const int CONTROL_PIN_1 = 27;
const int CONTROL_PIN_2 = 25;
bool relayState = true;

// ====== INTERRUPTS ======
void IRAM_ATTR countProd() { flowPulseProd++; }
void IRAM_ATTR countRech() { flowPulseRech++; }

// ================== FUNCIONES ==================

// Intento de inicializar Ethernet (DHCP)
void connectEthernet() {
  if (Ethernet.begin(mac) == 0) {
    Serial.println("[ETH] DHCP falló");
    connectedEthernet = false;
  } else {
    IPAddress ip = Ethernet.localIP();
    deviceIP = ip.toString();
    Serial.printf("[ETH] IP: %s\n", deviceIP.c_str());
    connectedEthernet = true;
  }
}

// Intento de conexión WiFi con parámetros (corrige bug y espera)
void connectWiFi(const char* ssid, const char* password, unsigned long timeoutMs = 10000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("[WiFi] Conectando a %s ...\n", ssid);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    deviceIP = WiFi.localIP().toString();
    Serial.printf("[WiFi] Conectado! IP: %s\n", deviceIP.c_str());
    connectedWiFi = true;
  } else {
    Serial.println("[WiFi] No se pudo conectar");
    connectedWiFi = false;
  }
}

// Intento de conectar con credenciales ya guardadas (WiFi.begin() sin argumentos)
// Devuelve true si quedó conectado rápidamente
bool tryConnectSavedWiFi(unsigned long timeoutMs = 8000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // intenta con credenciales persistentes en flash
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    deviceIP = WiFi.localIP().toString();
    connectedWiFi = true;
    Serial.printf("[WiFi] Conectado usando credenciales guardadas. IP: %s\n", deviceIP.c_str());
    return true;
  }
  connectedWiFi = false;
  return false;
}

void setupNetwork(const char* ssid, const char* password) {
  connectEthernet();

  if (!connectedEthernet) {
    Serial.println("[INFO] Intentando conexión WiFi como respaldo...");
    connectWiFi(ssid, password);

    if (!connectedWiFi) {
      Serial.println("[ERROR] Ninguna red disponible!");
      deviceIP = "Offline";
    }
  }
}


void checkResetPending() {
  if (reset_pending) {
    Serial.println("[INFO] Reinicio remoto activado...");
    SerialBT.println("[INFO] Reinicio remoto activado...");

    delay(500); // Pequeño delay antes de reinicio
    ESP.restart();
  }
}

String getControllerById() {
  if (jwtToken == "") {
    obtenerToken();
    delay(500);
    if (jwtToken == "") {
      Serial.println("[ERROR] No se pudo obtener token para GET controller");
      SerialBT.println("[ERROR] No se pudo obtener token para GET controller");
      return "";
    }
  }

  EthernetClient localClient;
  if (!localClient.connect(serverIp, serverPort)) {
    Serial.println("[ERROR] No se pudo conectar al servidor para GET controller");
    SerialBT.println("[ERROR] No se pudo conectar al servidor para GET controller");
    return "";
  }

  String request = "";
  request += "GET /api/v1.0/controllers/" + String(controllerId) + " HTTP/1.1\r\n";
  request += "Host: 164.92.95.176\r\n";
  request += "Authorization: Bearer " + jwtToken + "\r\n";
  request += "Connection: close\r\n\r\n";

  localClient.print(request);

  String response = "";
  unsigned long timeout = millis();
  while (localClient.connected() && millis() - timeout < 5000) {
    while (localClient.available()) response += (char)localClient.read();
  }
  localClient.stop();

  int jsonStart = response.indexOf("{");
  if (jsonStart == -1) return "";
  String json = response.substring(jsonStart);

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.println("[ERROR] No se pudo parsear JSON del controlador");
    SerialBT.println("[ERROR] No se pudo parsear JSON del controlador");
    return json;
  }

  // Actualización de kFactor y TDS
  if (doc.containsKey("kfactor_flujo")) {
    float newK = doc["kfactor_flujo"].as<float>();
    if (newK > 0) kFactor = newK;
  }
  if (doc.containsKey("kfactor_tds")) {
    float newTDS = doc["kfactor_tds"].as<float>();
    if (newTDS > 0) calibrationFactor = newTDS;
  }

  // ======== NUEVOS CAMPOS ========
  lapso_actualizacion = doc.containsKey("lapso_actualizacion") ? doc["lapso_actualizacion"].as<unsigned long>() : 60000;
  lapso_loop = doc.containsKey("lapso_loop") ? doc["lapso_loop"].as<unsigned long>() : 1000;
  reset_pending = doc.containsKey("reset_pending") ? doc["reset_pending"].as<bool>() : false;

  return json;
}

void setupPins() {
  pinMode(FLOW_PIN_PROD, INPUT_PULLUP);
  pinMode(FLOW_PIN_RECH, INPUT_PULLUP);
  pinMode(TDS_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_PROD), countProd, RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_RECH), countRech, RISING);

  pinMode(CONTROL_PIN_1, INPUT_PULLUP);
  pinMode(CONTROL_PIN_2, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Relay apagado (activo bajo)
  relayState = false;
}

void calcularFlujo() {
  unsigned long now = millis();
  unsigned long deltaTime = now - lastFlowCalc;  // tiempo transcurrido en ms

  if (deltaTime >= lapso_loop) {
    float deltaMinutes = deltaTime / 60000.0; // convertir ms a minutos

    flowRateProd = flowPulseProd / kFactor; // L/min
    flowRateRech = flowPulseRech / kFactor; // L/min

    volumenProd += flowRateProd * deltaMinutes; // L
    volumenRech += flowRateRech * deltaMinutes; // L

    flowPulseProd = 0;
    flowPulseRech = 0;
    lastFlowCalc = now;
  }
}


void updateRelay() {
  bool state27 = digitalRead(CONTROL_PIN_1);
  bool state25 = digitalRead(CONTROL_PIN_2);

  if (!state27 && !state25) {
    if (relayState) {
      relayState = false;
      digitalWrite(RELAY_PIN, HIGH);
    }
  } else {
    if (!relayState) {
      relayState = true;
      digitalWrite(RELAY_PIN, LOW);
    }
  }
}

void drawTableLayout() {
  tft.fillScreen(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);

  int rows = 6;
  int rowH = tft.height() / rows;
  tft.drawRect(0, 0, tft.width(), tft.height(), TFT_BLACK);

  for (int i = 1; i < rows; i++) {
    tft.drawLine(0, i * rowH, tft.width(), i * rowH, TFT_BLACK);
  }

  int colX = tft.width() * 0.50;
  tft.drawLine(colX, 0, colX, tft.height(), TFT_BLACK);

  tft.setCursor(5, rowH/2 - 8);        tft.print("Ajuste TDS");
  tft.setCursor(5, rowH + rowH/2 - 8); tft.print("TDS");
  tft.setCursor(5, 2*rowH + rowH/2 - 8); tft.print("V. Produccion");
  tft.setCursor(5, 3*rowH + rowH/2 - 8); tft.print("V. Rechazo");
  tft.setCursor(5, 4*rowH + rowH/2 - 8); tft.print("Bomba");
  tft.setCursor(5, 5*rowH + rowH/2 - 8); tft.print("Red");
}

void updateTable() {
  int rows = 6;
  int rowH = tft.height() / rows;
  int colX = tft.width() * 0.50 + 2;

  tft.setTextColor(TFT_BLUE, TFT_WHITE);
  tft.setCursor(colX, rowH/2 - 10);
  tft.printf("%.2f   ", calibrationFactor);

  tft.setTextColor(TFT_ORANGE, TFT_WHITE);
  tft.setCursor(colX, rowH + rowH/2 - 10);
  tft.printf("%5d ppm   ", tdsValue);

  tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
  tft.setCursor(colX, 2*rowH + rowH/2 - 10);
  tft.printf("%8.2f L/m", flowRateProd);

  tft.setTextColor(TFT_RED, TFT_WHITE);
  tft.setCursor(colX, 3*rowH + rowH/2 - 10);
  tft.printf("%8.2f L/m", flowRateRech);

  tft.setCursor(colX, 4*rowH + rowH/2 - 10);
  if (relayState) {
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.print("OFF");
  } else {
    tft.setTextColor(TFT_GREEN, TFT_WHITE);
    tft.print("ON ");
  }

  tft.setCursor(colX, 5*rowH + rowH/2 - 10);
  if (deviceIP != "") {
    tft.setTextColor(TFT_MAGENTA, TFT_WHITE);
    tft.print(deviceIP);
  } else {
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.print("Offline");
  }
}

#include <ArduinoJson.h>

void obtenerToken() {
  if (!client.connect(serverIp, serverPort)) {
    Serial.println("[ERROR] No se pudo conectar al servidor para login");
    return;
  }

  String loginPayload = "{\"email\":\"esp32@lcc.com.mx\",\"password\":\"Esp32*\"}";

  // === Enviar petición ===
  client.print("POST /api/v1.0/auth/login HTTP/1.1\r\n");
  client.print("Host: 164.92.95.176\r\n");   // cámbialo a dominio si tu backend lo requiere
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: ");
  client.println(loginPayload.length());
  client.print("Connection: close\r\n\r\n");
  client.print(loginPayload);

  // === Leer respuesta completa ===
  String response = "";
  unsigned long timeout = millis();
  while (millis() - timeout < 5000) {
    while (client.available()) {
      char c = client.read();
      response += c;
      timeout = millis(); // reinicia timeout mientras sigan llegando datos
    }
  }
  client.stop();

  Serial.println("=== Respuesta completa del servidor ===");
  Serial.println(response);
  Serial.println("=======================================");

  // === Separar headers del body JSON ===
  int bodyIndex = response.indexOf("\r\n\r\n");
  if (bodyIndex == -1) {
    Serial.println("[ERROR] No se encontró body en la respuesta");
    return;
  }
  String jsonBody = response.substring(bodyIndex + 4);

  Serial.println("=== JSON Body ===");
  Serial.println(jsonBody);
  Serial.println("=================");

  // === Parsear con ArduinoJson ===
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, jsonBody);

  if (!error) {
    if (doc.containsKey("token")) {
      jwtToken = doc["token"].as<String>();
      Serial.println("[OK] Token obtenido:");
      Serial.println(jwtToken);
      return;
    } else if (doc.containsKey("access_token")) {
      jwtToken = doc["access_token"].as<String>();
      Serial.println("[OK] Access Token obtenido:");
      Serial.println(jwtToken);
      return;
    } else {
      Serial.println("[WARN] JSON válido pero sin campo token");
    }
  } else {
    Serial.print("[ERROR] Fallo al parsear JSON: ");
    Serial.println(error.c_str());
  }

  // === Fallback con indexOf si ArduinoJson falla ===
  int tokenIndex = jsonBody.indexOf("\"token\":\"");
  if (tokenIndex != -1) {
    int start = tokenIndex + 9;
    int end = jsonBody.indexOf("\"", start);
    jwtToken = jsonBody.substring(start, end);
    Serial.println("[OK] Token obtenido con indexOf:");
    Serial.println(jwtToken);
  } else {
    Serial.println("[ERROR] No se encontró token en la respuesta");
  }
}


void enviarDatos(const char* productId, float flujoProd, float flujoRech, float tds, float temperature) {
  // 🔹 Obtener token si es necesario
  if (jwtToken == "") {
    obtenerToken();
    delay(200);
    if (jwtToken == "") return; // No se pudo obtener token
  }

  // 🔹 Leer sensores y actualizar flujos
  leerTDS();
  calcularFlujo();
  updateRelay();

  // 🔹 Calcular timestamps según lapso_loop
  unsigned long tiempo_fin_ms = millis();
  unsigned long tiempo_inicio_ms = (tiempo_fin_ms >= lapso_loop) ? tiempo_fin_ms - lapso_loop : 0;

  // 🔹 Crear JSON con ArduinoJson
  StaticJsonDocument<512> doc;
  doc["producto"] = productId;

  JsonObject realData = doc.createNestedObject("real_data");
  realData["flujo_produccion"] = flowRateProd;
  realData["flujo_rechazo"] = flowRateRech;
  realData["tds"] = tdsValue;
  realData["temperature"] = temperature;

  doc["tiempo_inicio"] = tiempo_inicio_ms;
  doc["tiempo_fin"] = tiempo_fin_ms;

  JsonObject pinStatus = doc.createNestedObject("pin_status");
  pinStatus["gpio15"] = digitalRead(FLOW_PIN_PROD);
  pinStatus["gpio4"] = digitalRead(FLOW_PIN_RECH);
  pinStatus["relay"] = relayState ? 1 : 0;

  String json;
  serializeJson(doc, json);

  // 🔹 Enviar HTTP POST por Ethernet
  EthernetClient localClient;
  if (!localClient.connect(serverIp, serverPort)) return;
  

  String request = "";
  request += "POST /api/v1.0/products/componentInput HTTP/1.1\r\n";
  request += "Host: 164.92.95.176\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Authorization: Bearer " + jwtToken + "\r\n";
  request += "Content-Length: " + String(json.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += json;

  Serial.println("========== REQUEST ==========");
  Serial.println(request);
  SerialBT.println(request);

  localClient.print(request);

  // 🔹 Leer respuesta del servidor
  String response = "";
  unsigned long timeout = millis();
  while (localClient.connected() && millis() - timeout < 5000) {
    while (localClient.available()) response += (char)localClient.read();
  }
  localClient.stop();

  // 🔹 Verificar expiración del token
  if (response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
    jwtToken = "";
  }
}

// ================== FUNCION: Manejar credenciales por Bluetooth ==================
void handleBluetoothWiFi() {
  static String credenciales = "";
  while (SerialBT.available()) {
    char c = SerialBT.read();
    // Soportar CR/LF y enter
    if (c == '\r') continue;
    if (c == '\n') {
      credenciales.trim();
      if (credenciales.length() == 0) {
        credenciales = "";
        return;
      }

      // Comando especial para borrar credenciales
      if (credenciales.equalsIgnoreCase("CLEAR_WIFI")) {
        Serial.println("[BT] Comando CLEAR_WIFI recibido -> borrando credenciales guardadas...");
        SerialBT.println("[BT] CLEAR_WIFI -> borrando credenciales...");
        // Borra credenciales persistentes
        WiFi.disconnect(true, true); // borra configuración y credenciales en flash
        delay(500);
        Serial.println("[BT] Credenciales borradas. Reiniciando...");
        SerialBT.println("[BT] Credenciales borradas. Reiniciando...");
        delay(500);
        ESP.restart();
        // no regresa
      }

      int comaIndex = credenciales.indexOf(',');
      if (comaIndex > 0) {
        String ssid = credenciales.substring(0, comaIndex);
        String password = credenciales.substring(comaIndex + 1);

        ssid.trim();
        password.trim();

        Serial.printf("[BT] Recibido SSID: '%s'  PASS: '%s'\n", ssid.c_str(), password.c_str());
        SerialBT.printf("[BT] Intentando conectar a: %s\n", ssid.c_str());

        // Intento de conexión
        WiFi.mode(WIFI_STA);
        WiFi.persistent(true);               // activar persistencia antes de begin
        WiFi.begin(ssid.c_str(), password.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
          delay(500);
          Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
          deviceIP = WiFi.localIP().toString();
          connectedWiFi = true;

          Serial.printf("[WiFi] Conectado! IP: %s\n", deviceIP.c_str());
          SerialBT.printf("[WiFi] Conectado! IP: %s\n", deviceIP.c_str());

          tft.fillScreen(TFT_WHITE);
          tft.setCursor(10, tft.height()/2 - 10);
          tft.setTextColor(TFT_BLACK, TFT_WHITE);
          tft.printf("WiFi conectado: %s", deviceIP.c_str());

          delay(1500); // deja que el usuario vea el mensaje

          // Reiniciar para que el dispositivo arranque con credenciales persistentes
          Serial.println("[INFO] Reiniciando para aplicar configuración persistente...");
          SerialBT.println("[INFO] Reiniciando para aplicar configuración persistente...");
          delay(300);
          ESP.restart(); // al reiniciar, el ESP intentará conectarse automáticamente con las credenciales guardadas
        } else {
          Serial.println("[WiFi] No se pudo conectar con esas credenciales.");
          SerialBT.println("[WiFi] No se pudo conectar con esas credenciales.");
          // Desactivar persistencia si la hubo para evitar guardar credenciales inválidas
          WiFi.persistent(false);
        }
      } else {
        SerialBT.println("[ERROR] Formato inválido. Usa: NOMBRE_RED,PASSWORD_RED");
      }
      credenciales = "";
    } else {
      credenciales += c;
      // mantener buffer razonable
      if (credenciales.length() > 200) credenciales = credenciales.substring(0, 200);
    }
  }
}

// ====== SETUP ======
void setup() {
  // ====== Serial y Bluetooth ======
  Serial.begin(115200);
  SerialBT.begin("ESP32_TDS_Flujo");
  Serial.println("[INFO] Iniciando ESP32_TDS_Flujo...");

  // ====== Configuración de pines ======
  setupPins();

  // ====== TFT ======
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);

  // Forzar orientación y colores correctos
  tft.writecommand(0x36);  // MADCTL
  tft.writedata(0x28);

  tft.fillScreen(TFT_WHITE);
  tft.setCursor(10, tft.height()/2 - 10);
  tft.print("Iniciando componente...");
  delay(800);

  tft.fillScreen(TFT_WHITE);
  tft.setCursor(10, tft.height()/2 - 10);
  tft.print("Obteniendo IP ...");

  // ====== ADC TDS ======
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);

  // ====== Reset y pin INT ======
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(200);
  digitalWrite(PIN_RST, HIGH);
  delay(200);
  pinMode(PIN_INT, INPUT);

  // ====== Inicializar SPI y Ethernet ======
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);

  // ====== Conexión a red con fallback ======

  // Intentar Ethernet primero
  connectEthernet();

  if (!connectedEthernet) {
    Serial.println("[INFO] Ethernet no disponible, intento WiFi con credenciales guardadas...");
    tft.fillScreen(TFT_WHITE);
    tft.setCursor(10, tft.height()/2 - 20);
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.print("Sin red!");
    tft.setCursor(10, tft.height()/2);
    tft.setTextColor(TFT_BLUE, TFT_WHITE);
    tft.print("Si ya configuraste BT, espera...");

    // Intentar conectarse con credenciales persistentes si existen
    if (tryConnectSavedWiFi(8000)) {
      // conectado vía WiFi persistente
      tft.fillScreen(TFT_WHITE);
      tft.setCursor(10, tft.height()/2 - 10);
      tft.setTextColor(TFT_BLACK, TFT_WHITE);
      tft.printf("IP: %s", deviceIP.c_str());
    } else {
      // No hay credenciales guardadas o fallo al conectar
      Serial.println("[INFO] No hay credenciales guardadas o falló conexión. Esperando Bluetooth...");
      tft.fillScreen(TFT_WHITE);
      tft.setCursor(10, tft.height()/2 - 20);
      tft.setTextColor(TFT_RED, TFT_WHITE);
      tft.print("No hay red!");
      tft.setCursor(10, tft.height()/2);
      tft.setTextColor(TFT_BLUE, TFT_WHITE);
      tft.print("Envía SSID,PASSWORD por BT");
      deviceIP = "Offline";
    }
  } else {
    Serial.printf("[INFO] Ethernet conectado. IP: %s\n", deviceIP.c_str());
    tft.fillScreen(TFT_WHITE);
    tft.setCursor(10, tft.height()/2 - 10);
    tft.setTextColor(TFT_BLACK, TFT_WHITE);
    tft.printf("IP: %s", deviceIP.c_str());
  }

  // ====== Configuración NTP ======
  if (connectedEthernet || connectedWiFi) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println("[NTP] Hora sincronizada.");
    } else {
      Serial.println("[NTP] No se pudo sincronizar NTP (aún).");
    }
  }

  // ====== Inicializar tabla y obtener token (si hay red) ======
  drawTableLayout();
  updateTable();

  if (connectedEthernet || connectedWiFi) {
    obtenerToken();       // intenta conseguir token inicial
    // obtener datos del controller inicial
    String controllerData = getControllerById();
    (void)controllerData;
    updateTable();
  } else {
    Serial.println("[INFO] Sin red - token y controller aplazados hasta conexión.");
  }
}


// ====== LOOP ======
void loop() {
  unsigned long now = millis();

  // 🔹 Actualizar datos del controller según lapso_actualizacion (solo si hay red)
  if ((connectedEthernet || connectedWiFi) && (now - lastUpdateController >= lapso_actualizacion)) {
    String controllerData = getControllerById();
    lastUpdateController = now;
    updateTable();
  }

  // 🔹 Leer sensores, calcular flujos y actualizar pantalla según lapso_loop
  if (now - lastLoop >= lapso_loop) {
    lastLoop = now;

    leerTDS();
    calcularFlujo();
    updateRelay();
    updateTable();

    if (connectedEthernet || connectedWiFi) {
      if (jwtToken == "") {
        obtenerToken();
        delay(200);
      }
      enviarDatos(productId, flowRateProd, flowRateRech, tdsValue, 25.6);
    }
  }

  // 🔹 Revisar reinicio remoto siempre
  checkResetPending();

  // 🔹 Escuchar credenciales WiFi por Bluetooth (si no hay red)
  if (!connectedEthernet && !connectedWiFi) {
    handleBluetoothWiFi();
  } else {
    // Si hay red, todavía aceptamos CLEAR_WIFI por BT para borrar credenciales si el usuario lo solicita
    if (SerialBT.available()) {
      // leer posible comando CLEAR_WIFI
      String cmd = SerialBT.readStringUntil('\n');
      cmd.trim();
      if (cmd.equalsIgnoreCase("CLEAR_WIFI")) {
        Serial.println("[BT] Comando CLEAR_WIFI recibido -> borrando credenciales guardadas...");
        SerialBT.println("[BT] CLEAR_WIFI -> borrando credenciales...");
        WiFi.disconnect(true, true);
        delay(400);
        ESP.restart();
      }
      // si llega otra cosa por BT, ignoramos (o podrías usarlo para debug)
    }
  }
}

