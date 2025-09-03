Todos Componentes unidos — (AJUSTANDO VALORES CONVERSION)
#include <SPI.h>
#include <Ethernet.h>
#include <TFT_eSPI.h>
#include "BluetoothSerial.h"
#include <driver/adc.h>   // Para atenuación ADC en ESP32
#include <time.h>   // ⬅️ Para NTP

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
String deviceIP = "0.0.0.0";

// ================== CONFIGURACIÓN NTP ==================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -7 * 3600;   // Hermosillo = UTC-7
const int daylightOffset_sec = 0;       // No DST

// ================== SENSOR TDS ==================
BluetoothSerial SerialBT;
TFT_eSPI tft = TFT_eSPI();

const int TDS_PIN = 32;
float calibrationFactor = 0.50f;
float voltageOffset     = 0.20f;
const int NUM_SAMPLES   = 20;


int tdsRaw = 0;
int tdsValue = 0;
float lastVoltage = 0.0f;

// ================== FLUJOS ==================
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
bool relayState = false;

// ====== INTERRUPTS ======
void IRAM_ATTR countProd() { flowPulseProd++; }
void IRAM_ATTR countRech() { flowPulseRech++; }

// ================== FUNCIONES ==================
int readADC_Average(int pin, int samples) {
  long acc = 0;
  for (int i = 0; i < samples; i++) {
    acc += analogRead(pin);
    delayMicroseconds(500);
  }
  return (int)(acc / samples);
}

void leerTDS() {
  int raw = readADC_Average(TDS_PIN, NUM_SAMPLES);

  float voltage = (raw / 4095.0f) * 3.3f;
  lastVoltage = voltage;

  float vCal = voltage - voltageOffset;
  if (vCal < 0) vCal = 0;

  float tds_ppm_raw = (133.42f * powf(vCal, 3)
                     - 255.86f * powf(vCal, 2)
                     + 857.39f * vCal);

  float tds_ppm_cal = tds_ppm_raw * calibrationFactor;

  tdsRaw = (int)tds_ppm_raw;
  tdsValue = (int)tds_ppm_cal;
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

// ====== CALCULAR FLUJOS ======
void calcularFlujo() {
  unsigned long now = millis();
  if (now - lastFlowCalc >= 1000) {
    float kFactor = 32.0625; // Ajuste sensor

    flowRateProd = (flowPulseProd / kFactor) * 60.0;
    flowRateRech = (flowPulseRech / kFactor) * 60.0;

    volumenProd += flowRateProd / 60.0; // litros acumulados
    volumenRech += flowRateRech / 60.0;

    flowPulseProd = 0;
    flowPulseRech = 0;
    lastFlowCalc = now;
  }
}

// ====== RELAY ======
void updateRelay() {
  bool state27 = digitalRead(CONTROL_PIN_1);
  bool state25 = digitalRead(CONTROL_PIN_2);

  if (!state27 && !state25) { // Ambos presionados, apagar
    if (relayState) {
      relayState = false;
      digitalWrite(RELAY_PIN, HIGH);
    }
  } else { // uno o ambos sueltos, encender
    if (!relayState) {
      relayState = true;
      digitalWrite(RELAY_PIN, LOW);
    }
  }
}

// ====== DISPLAY ======
void drawTableLayout() {
  tft.fillScreen(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);

  int rows = 6;
  int rowH = tft.height() / rows;

  // Marco principal
  tft.drawRect(0, 0, tft.width(), tft.height(), TFT_BLACK);

  // Líneas horizontales
  for (int i = 1; i < rows; i++) {
    tft.drawLine(0, i * rowH, tft.width(), i * rowH, TFT_BLACK);
  }

  // Línea vertical etiqueta/valor (📌 un poco más a la izquierda)
  int colX = tft.width() * 0.50;  
  tft.drawLine(colX, 0, colX, tft.height(), TFT_BLACK);

  // Etiquetas
  tft.setCursor(5, rowH/2 - 8);        tft.print("TDS Crudo");
  tft.setCursor(5, rowH + rowH/2 - 8); tft.print("TDS Ajustado");
  tft.setCursor(5, 2*rowH + rowH/2 - 8); tft.print("V. Produccion");
  tft.setCursor(5, 3*rowH + rowH/2 - 8); tft.print("V. Rechazo");
  tft.setCursor(5, 4*rowH + rowH/2 - 8); tft.print("Bomba");
  tft.setCursor(5, 5*rowH + rowH/2 - 8); tft.print("Red");
}


void updateTable() {
  int rows = 6;
  int rowH = tft.height() / rows;
  int colX = tft.width() * 0.50 + 2;

  // TDS Crudo
  tft.setTextColor(TFT_BLUE, TFT_WHITE);
  tft.setCursor(colX, rowH/2 - 10); 
  tft.printf("%5d ppm   ", tdsRaw);

  // TDS Ajustado
  tft.setTextColor(TFT_ORANGE, TFT_WHITE);
  tft.setCursor(colX, rowH + rowH/2 - 10); 
  tft.printf("%5d ppm   ", tdsValue);

  // Volumen Producción
  tft.setTextColor(TFT_DARKGREEN, TFT_WHITE);
  tft.setCursor(colX, 2*rowH + rowH/2 - 10); 
  tft.printf("%8.2f L/m", flowRateProd);

  // Volumen Rechazo
  tft.setTextColor(TFT_RED, TFT_WHITE);
  tft.setCursor(colX, 3*rowH + rowH/2 - 10); 
  tft.printf("%8.2f L/m", flowRateRech);

  // Estado Bomba
  tft.setCursor(colX, 4*rowH + rowH/2 - 10);
  if (relayState) {
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.print("OFF");
  } else {
    tft.setTextColor(TFT_GREEN, TFT_WHITE);
    tft.print("ON ");
  }

  // Estado Red (mostrar IP si existe)
  tft.setCursor(colX, 5*rowH + rowH/2 - 10);
  if (deviceIP != "") {
    tft.setTextColor(TFT_MAGENTA, TFT_WHITE);
    tft.print(deviceIP);
  } else {
    tft.setTextColor(TFT_RED, TFT_WHITE);
    tft.print("Offline");
  }
}




// ====== ETHERNET ======
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
  // Asegurarse de que tenemos token
  if (jwtToken == "") {
    obtenerToken();
    delay(500); // esperar un poco por la respuesta
    if (jwtToken == "") {
      Serial.println("[ERROR] No se pudo obtener token, se omite envío de datos.");
      return;
    }
  }

  // Obtener timestamps (epoch en ms)
  time_t ahora = time(nullptr);
  long tiempo_inicio = (long)ahora * 1000;
  long tiempo_fin    = (long)(ahora + 1) * 1000; // simula 1s después

  // Leer sensores
  leerTDS();
  calcularFlujo();
  updateRelay();

  // Crear cliente local para evitar conflictos
  EthernetClient localClient;
  if (!localClient.connect(serverIp, serverPort)) {
    Serial.println("[ERROR] No se pudo conectar al servidor en enviarDatos()");
    return;
  }

  // Construir JSON
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

  // Construcción del request
  String request = "";
  request += "POST /api/v1.0/products/componentInput HTTP/1.1\r\n";
  request += "Host: 164.92.95.176\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Authorization: Bearer " + jwtToken + "\r\n";
  request += "Content-Length: " + String(json.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += json;

  // Log del request
  Serial.println("========== REQUEST ==========");
  Serial.println(request);
  Serial.println("========== END REQUEST ==========");

  localClient.print(request);

  // Leer respuesta
  String response = "";
  unsigned long timeout = millis();
  while (localClient.connected() && millis() - timeout < 5000) {
    while (localClient.available()) response += (char)localClient.read();
  }
  localClient.stop();

  // Log del response
  Serial.println("========== RESPONSE ==========");
  Serial.println(response);
  Serial.println("========== END RESPONSE ==========");

  // Verificar token
  if (response.indexOf("invalid") != -1 || response.indexOf("expired") != -1) {
    jwtToken = "";
    Serial.println("[WARN] Token inválido o expirado. Se solicitará uno nuevo.");
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

  // Forzar orientación
  tft.writecommand(0x36);
  tft.writedata(0x28);

  drawTableLayout();

  // ADC calibración
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);

  // Ethernet
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(200);
  digitalWrite(PIN_RST, HIGH);
  delay(200);
  pinMode(PIN_INT, INPUT);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  Ethernet.init(PIN_CS);

  if (Ethernet.begin(mac) == 0) {
    Serial.println("Error DHCP");
    while (true);
  }
  delay(1000);
  IPAddress ip = Ethernet.localIP();
  deviceIP = ip.toString();   // ✅ guardamos en variable global
  Serial.print("IP asignada: ");
  Serial.println(deviceIP);
  // Configuración de NTP con zona horaria Hermosillo
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("⏳ Sincronizando hora NTP...");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.print("✅ Hora local: ");
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
  } else {
    Serial.println("❌ Error al obtener hora NTP");
  }
  obtenerToken();
}

// ====== LOOP ======
void loop() {
  leerTDS();
  calcularFlujo();
  updateRelay();
  updateTable();

  Serial.printf("TDS: %d | TDScal: %d | Prod: %.2f | Rech: %.2f | Relay: %d\n",
                tdsRaw, tdsValue, flowRateProd, flowRateRech, relayState);
  SerialBT.printf("TDS: %d | TDScal: %d | Prod: %.2f | Rech: %.2f | Relay: %d\n",
                  tdsRaw, tdsValue, flowRateProd, flowRateRech, relayState);

  if (jwtToken == "") {
    obtenerToken();
    delay(5000);
  }

  enviarDatos(productId, flowRateProd, flowRateRech, tdsValue, 25.6);

  delay(1000);
}
