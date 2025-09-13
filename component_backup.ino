#include <SPI.h>
#include <UIPEthernet.h>  
#include <TFT_eSPI.h>
#include "BluetoothSerial.h"
#include <driver/adc.h>   // Para atenuación ADC en ESP32
#include <time.h>   // ⬅️ Para NTP
#include <ArduinoJson.h>

// ================== RED ETHERNET ==================
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
const char* controllerId = "68ba69fe76b0079735dbe918";

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

  if (doc.containsKey("kfactor_flujo")) {
    float newK = doc["kfactor_flujo"].as<float>();
    if (newK > 0) kFactor = newK;
  }

  if (doc.containsKey("kfactor_tds")) {
    float newTDS = doc["kfactor_tds"].as<float>();
    if (newTDS > 0) calibrationFactor = newTDS;
  }

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
  if (now - lastFlowCalc >= 1000) {    
    flowRateProd = flowPulseProd / kFactor; // L/min
    flowRateRech = flowPulseRech / kFactor; // L/min
    
    volumenProd += flowRateProd / 60.0;
    volumenRech += flowRateRech / 60.0;

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

void obtenerToken() {
  if (!client.connect(serverIp, serverPort)) return;

  String loginPayload = "{\"email\":\"esp32@lcc.com.mx\",\"password\":\"Esp32*\"}";

  client.print("POST /api/v1.0/auth/login HTTP/1.1\r\n");
  client.print("Host: 164.92.95.176\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: ");
  client.print(loginPayload.length());
  client.print("\r\nConnection: close\r\n\r\n");
  client.print(loginPayload);

  String response = "";
  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 5000) {
    while (client.available()) response += (char)client.read();
  }
  client.stop();

  int tokenIndex = response.indexOf("\"token\":\"");
  if (tokenIndex != -1) {
    int start = tokenIndex + 9;
    int end = response.indexOf("\"", start);
    jwtToken = response.substring(start, end);
  }
}

void enviarDatos(const char* productId, float flujoProd, float flujoRech, float tds, float temperature) {
  if (jwtToken == "") {
    obtenerToken();
    delay(500);
    if (jwtToken == "") return;
  }

  time_t ahora = time(nullptr);
  long tiempo_inicio = (long)ahora * 1000;
  long tiempo_fin    = (long)(ahora + 1) * 1000;

  leerTDS();
  calcularFlujo();
  updateRelay();

  EthernetClient localClient;
  if (!localClient.connect(serverIp, serverPort)) return;

  String json = "{";
  json += "\"producto\":\"" + String(productId) + "\",";
  json += "\"real_data\":{";
  json += "\"flujo_produccion\":" + String(flowRateProd, 2) + ",";
  json += "\"flujo_rechazo\":" + String(flowRateRech, 2) + ",";
  json += "\"tds\":" + String(tdsValue) + ",";
  json += "\"temperature\":" + String(25.6, 1);
  json += "},";
  json += "\"tiempo_inicio\":" + String(tiempo_inicio) + ",";
  json += "\"tiempo_fin\":" + String(tiempo_fin) + ",";
  json += "\"pin_status\":{";
  json += "\"gpio15\":" + String(digitalRead(FLOW_PIN_PROD)) + ",";
  json += "\"gpio4\":" + String(digitalRead(FLOW_PIN_RECH)) + ",";
  json += "\"relay\":" + String(relayState ? 1 : 0);
  json += "}";
  json += "}";

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

  String response = "";
  unsigned long timeout = millis();
  while (localClient.connected() && millis() - timeout < 5000) {
    while (localClient.available()) response += (char)localClient.read();
  }
  localClient.stop();

  if (response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
    jwtToken = "";
  }
}

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  SerialBT.begin("ESP32_TDS_Flujo");

  setupPins();

  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);

  // 🔹 Forzar orientación y colores correctos
  tft.writecommand(0x36);  // MADCTL
  tft.writedata(0x28);     // Invierte RGB/BGR y orientación según tu pantalla

  tft.fillScreen(TFT_WHITE);
  tft.setCursor(10, tft.height()/2 - 10);
  tft.print("Iniciando componente...");
  delay(1000);

  tft.fillScreen(TFT_WHITE);
  tft.setCursor(10, tft.height()/2 - 10);
  tft.print("Obteniendo IP ...");

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);

  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(200);
  digitalWrite(PIN_RST, HIGH);
  delay(200);
  pinMode(PIN_INT, INPUT);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);

  if (Ethernet.begin(mac) == 0) {
    tft.fillScreen(TFT_WHITE);
    tft.setCursor(10, tft.height()/2 - 10);
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.print("Error DHCP");
    while (true);
  }

  delay(1000);
  IPAddress ip = Ethernet.localIP();
  deviceIP = ip.toString();

  tft.fillScreen(TFT_WHITE);
  tft.setCursor(10, tft.height()/2 - 10);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.printf("IP obtenida: %s", deviceIP.c_str());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  String controllerData = getControllerById();

  drawTableLayout();
  updateTable();

  obtenerToken();
}

// ====== LOOP ======
void loop() {
  leerTDS();
  calcularFlujo();
  updateRelay();
  updateTable();

  if (jwtToken == "") {
    obtenerToken();
    delay(5000);
  }

  enviarDatos(productId, flowRateProd, flowRateRech, tdsValue, 25.6);
  SerialBT.printf("[BT] Prod: %.2f L/min | Rech: %.2f L/min | Vprod: %.3f L | Vrech: %.3f L | Relay: %d\n",
                  flowRateProd, flowRateRech, volumenProd, volumenRech, relayState);

  delay(1000);
}
