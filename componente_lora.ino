// ================== ESP32 OLED WiFi & LoRa Monitor ==================
// Sketch para mostrar estado WiFi y escanear dispositivos LoRa

#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LoRa.h>
// Librería PubSubClient para MQTT
// Instalar desde: Sketch → Include Library → Manage Libraries → Buscar "PubSubClient" (Nick O'Leary)
// O desde: https://github.com/knolleary/pubsubclient
#include <PubSubClient.h>

// ================== WiFi Configuration ==================
const char* ssid = "BochoLandStarlink2.4";
const char* password = "Sakaunstarlink24*";

// ================== UDP Configuration ==================
WiFiUDP udp;
const int udpPort = 12345;  // Puerto UDP para recibir datos

// ================== MQTT Configuration ==================
const char* mqtt_server = "146.190.143.141";
const int mqtt_port = 1883;
const char* mqtt_client_id = "ESP32_LoRa_Receiver";
const char* mqtt_topic_presion_in = "aquatech/presion_in";
const char* mqtt_topic_presion_out = "aquatech/presion_out";
const char* mqtt_topic_status = "aquatech/status";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ================== LoRa Configuration ==================
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_CS 18
#define LORA_RST 14
#define LORA_DIO0 26
#define LORA_FREQUENCY 915E6  // 915 MHz

// ================== OLED Configuration ==================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

// Configuraciones de pines comunes (se probarán automáticamente)
struct OLEDConfig {
  int sda;
  int scl;
  int rst;
};

OLEDConfig configs[] = {
  {4, 15, 16},   // TTGO LoRa32 / Heltec LoRa32
  {21, 22, -1},  // Estándar ESP32
  {21, 22, 16},  // Estándar con RST
};

uint8_t addresses[] = {0x3C, 0x3D};

// Variables globales
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
int OLED_SDA = 21;
int OLED_SCL = 22;
int OLED_RST = -1;
uint8_t OLED_ADDR = 0x3C;
bool displayInitialized = false;

// Estado del sistema
bool wifiConnected = false;
bool loraInitialized = false;
bool mqttConnected = false;
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 500; // Actualizar cada 500ms
unsigned long lastMqttReconnect = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000; // Intentar reconectar cada 5 segundos

// Datos recibidos vía LoRa o WiFi
float presion_in = 0.0;
float presion_out = 0.0;
unsigned long lastDataReceived = 0;
int packetCount = 0;
String dataSource = "None"; // "LoRa" o "WiFi"

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==========================================");
  Serial.println("ESP32 WiFi & LoRa Monitor");
  Serial.println("==========================================\n");
  
  // Detectar y inicializar OLED
  if (initOLED()) {
    displayInitialized = true;
    Serial.println("✅ Display inicializado correctamente!");
    
    // Mostrar mensaje inicial
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Inicializando...");
    display.setCursor(0, 10);
    display.println("Display OK");
    display.display();
    delay(1000);
  } else {
    Serial.println("❌ Error: No se pudo inicializar el display");
    displayInitialized = false;
  }
  
  // Inicializar LoRa
  Serial.println("\nInicializando LoRa...");
  if (initLoRa()) {
    loraInitialized = true;
    Serial.println("✅ LoRa inicializado correctamente!");
  } else {
    loraInitialized = false;
    Serial.println("⚠️  LoRa no inicializado (continuando sin LoRa)");
  }
  
  // Conectar a WiFi
  Serial.println("\nConectando a WiFi...");
  connectToWiFi();
  
  // Inicializar MQTT
  if (wifiConnected) {
    Serial.println("\nInicializando MQTT...");
    initMQTT();
  }
  
  Serial.println("\nSistema listo.\n");
}

// ================== Loop ==================
void loop() {
  if (!displayInitialized) {
    delay(1000);
    return;
  }
  
  // Verificar estado WiFi periódicamente
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck >= 5000) {
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("[WiFi] Desconectado, intentando reconectar...");
      connectToWiFi();
    } else if (!wifiConnected) {
      wifiConnected = true;
      Serial.println("[WiFi] Reconectado!");
    }
    lastWiFiCheck = millis();
  }
  
  // Escanear dispositivos LoRa si está inicializado
  if (loraInitialized) {
    scanLoRaDevices();
  }
  
  // Recibir datos WiFi UDP si está conectado
  if (wifiConnected) {
    receiveWiFiData();
  }
  
  // Manejar MQTT
  if (wifiConnected) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    } else {
      mqttClient.loop(); // Mantener conexión MQTT activa
    }
  }
  
  // Actualizar display
  if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    updateDisplay();
    lastDisplayUpdate = millis();
  }
  
  delay(100);
}

// ================== Display Functions ==================
void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  
  // Línea 1: Estado WiFi
  display.setCursor(0, 0);
  display.print("WiFi: ");
  if (wifiConnected) {
    display.print("OK");
    display.print(" ");
    display.print(WiFi.RSSI());
    display.print("dBm");
  } else {
    display.print("NO");
  }
  
  // Línea 2: IP Address y MQTT
  display.setCursor(0, 10);
  if (wifiConnected) {
    display.print("IP: ");
    IPAddress ip = WiFi.localIP();
    display.print(ip[0]);
    display.print(".");
    display.print(ip[1]);
    display.print(".");
    display.print(ip[2]);
    display.print(".");
    display.print(ip[3]);
    display.print(" MQTT:");
    display.print(mqttConnected ? "OK" : "NO");
  } else {
    display.print("IP: 0.0.0.0");
  }
  
  // Línea 3: Estado LoRa / Fuente de datos
  display.setCursor(0, 20);
  if (millis() - lastDataReceived < 5000) {
    display.print("Data: ");
    display.print(dataSource);
  } else {
    display.print("LoRa: ");
    if (loraInitialized) {
      display.print("OK 915MHz");
    } else {
      display.print("NO");
    }
  }
  
  // Línea 4: Tiempo transcurrido
  display.setCursor(0, 30);
  display.print("Time: ");
  
  unsigned long seconds = millis() / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  seconds = seconds % 60;
  minutes = minutes % 60;
  
  if (hours < 10) display.print("0");
  display.print(hours);
  display.print(":");
  if (minutes < 10) display.print("0");
  display.print(minutes);
  display.print(":");
  if (seconds < 10) display.print("0");
  display.print(seconds);
  
  // Línea 5: Presión IN
  display.setCursor(0, 40);
  display.print("P_IN: ");
  if (millis() - lastDataReceived < 5000) { // Si recibió datos en los últimos 5 segundos
    display.print(presion_in, 1);
  } else {
    display.print("---");
  }
  
  // Línea 6: Presión OUT
  display.setCursor(0, 50);
  display.print("P_OUT: ");
  if (millis() - lastDataReceived < 5000) { // Si recibió datos en los últimos 5 segundos
    display.print(presion_out, 1);
    display.print(" | #");
    display.print(packetCount);
  } else {
    display.print("---");
    display.print(" | Waiting...");
  }
  
  // Actualizar display
  display.display();
}

// ================== OLED Initialization ==================
bool initOLED() {
  Serial.println("Inicializando OLED...");
  
  int numConfigs = sizeof(configs) / sizeof(configs[0]);
  int numAddresses = sizeof(addresses) / sizeof(addresses[0]);
  
  for (int i = 0; i < numConfigs; i++) {
    for (int j = 0; j < numAddresses; j++) {
      Serial.print("Probando SDA=");
      Serial.print(configs[i].sda);
      Serial.print(", SCL=");
      Serial.print(configs[i].scl);
      Serial.print(", RST=");
      Serial.print(configs[i].rst);
      Serial.print(", Addr=0x");
      Serial.println(addresses[j], HEX);
      
      // Configurar Wire
      Wire.end();
      delay(50);
      Wire.begin(configs[i].sda, configs[i].scl);
      delay(100);
      
      // Reset si es necesario
      if (configs[i].rst >= 0) {
        pinMode(configs[i].rst, OUTPUT);
        digitalWrite(configs[i].rst, LOW);
        delay(50);
        digitalWrite(configs[i].rst, HIGH);
        delay(100);
      }
      
      // Intentar inicializar
      if (display.begin(SSD1306_SWITCHCAPVCC, addresses[j])) {
        OLED_SDA = configs[i].sda;
        OLED_SCL = configs[i].scl;
        OLED_RST = configs[i].rst;
        OLED_ADDR = addresses[j];
        
        Serial.print("✅ Display encontrado! SDA=");
        Serial.print(OLED_SDA);
        Serial.print(", SCL=");
        Serial.print(OLED_SCL);
        Serial.print(", Addr=0x");
        Serial.println(OLED_ADDR, HEX);
        
        return true;
      }
      delay(100);
    }
  }
  
  return false;
}

// ================== WiFi Functions ==================
void connectToWiFi() {
  Serial.print("Conectando a: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("✅ WiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    
    // Inicializar UDP para recibir datos
    udp.begin(udpPort);
    Serial.print("UDP iniciado en puerto ");
    Serial.println(udpPort);
  } else {
    wifiConnected = false;
    Serial.println("❌ Error: No se pudo conectar a WiFi");
  }
}

// ================== LoRa Functions ==================
bool initLoRa() {
  Serial.println("Inicializando SPI para LoRa...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  delay(100);
  
  Serial.println("Configurando pines LoRa...");
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("❌ Error al inicializar LoRa");
    Serial.println("Verifica las conexiones SPI");
    return false;
  }
  
  // Configurar parámetros LoRa
  LoRa.setSpreadingFactor(7);      // SF7
  LoRa.setSignalBandwidth(125E3);   // 125 kHz
  LoRa.setCodingRate4(5);           // CR 4/5
  LoRa.setPreambleLength(8);        // Preamble length
  LoRa.enableCrc();                 // Habilitar CRC
  
  Serial.print("✅ LoRa inicializado en ");
  Serial.print(LORA_FREQUENCY / 1E6);
  Serial.println(" MHz");
  
  return true;
}

void scanLoRaDevices() {
  // Escanear paquetes LoRa disponibles
  int packetSize = LoRa.parsePacket();
  
  if (packetSize) {
    packetCount++;
    lastDataReceived = millis();
    dataSource = "LoRa";
    
    // Leer datos del paquete
    String data = "";
    while (LoRa.available()) {
      data += (char)LoRa.read();
    }
    
    Serial.print("[LoRa] Paquete #");
    Serial.print(packetCount);
    Serial.print(" | RSSI: ");
    Serial.print(LoRa.packetRssi());
    Serial.print(" dBm | Datos: ");
    Serial.println(data);
    
    // Parsear datos: "presion_in:XX.X,presion_out:YY.Y"
    if (data.length() > 0) {
      parseLoRaData(data);
    }
  }
}

void parseLoRaData(String data) {
  // Buscar presion_in
  int inIndex = data.indexOf("presion_in:");
  if (inIndex >= 0) {
    int inStart = inIndex + 11; // "presion_in:" tiene 11 caracteres
    int inEnd = data.indexOf(",", inStart);
    if (inEnd < 0) inEnd = data.length();
    String inValue = data.substring(inStart, inEnd);
    presion_in = inValue.toFloat();
  }
  
  // Buscar presion_out
  int outIndex = data.indexOf("presion_out:");
  if (outIndex >= 0) {
    int outStart = outIndex + 12; // "presion_out:" tiene 12 caracteres
    int outEnd = data.length();
    String outValue = data.substring(outStart, outEnd);
    presion_out = outValue.toFloat();
  }
  
  Serial.print("  -> Presion IN: ");
  Serial.print(presion_in, 1);
  Serial.print(" | Presion OUT: ");
  Serial.println(presion_out, 1);
  
  // Publicar a MQTT si está conectado
  if (mqttClient.connected()) {
    publishToMQTT();
  }
}

// ================== WiFi UDP Functions ==================
void receiveWiFiData() {
  int packetSize = udp.parsePacket();
  
  if (packetSize) {
    packetCount++;
    lastDataReceived = millis();
    dataSource = "WiFi";
    
    // Leer datos del paquete UDP
    String data = "";
    while (udp.available()) {
      data += (char)udp.read();
    }
    
    Serial.print("[WiFi] Paquete #");
    Serial.print(packetCount);
    Serial.print(" desde ");
    Serial.print(udp.remoteIP());
    Serial.print(" | Datos: ");
    Serial.println(data);
    
    // Parsear datos (mismo formato que LoRa)
    if (data.length() > 0) {
      parseLoRaData(data); // Reutilizar la misma función de parsing
    }
  }
}

// ================== MQTT Functions ==================
void initMQTT() {
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  reconnectMQTT();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Callback para mensajes recibidos (si es necesario)
  Serial.print("[MQTT] Mensaje recibido en topic: ");
  Serial.print(topic);
  Serial.print(" | Payload: ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

void reconnectMQTT() {
  // Intentar reconectar solo cada 5 segundos
  if (millis() - lastMqttReconnect < MQTT_RECONNECT_INTERVAL) {
    return;
  }
  lastMqttReconnect = millis();
  
  if (!mqttClient.connected() && wifiConnected) {
    Serial.print("Conectando a MQTT: ");
    Serial.print(mqtt_server);
    Serial.print(":");
    Serial.println(mqtt_port);
    
    if (mqttClient.connect(mqtt_client_id)) {
      mqttConnected = true;
      Serial.println("✅ MQTT conectado!");
      
      // Publicar mensaje de estado
      String statusMsg = "{\"status\":\"online\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
      mqttClient.publish(mqtt_topic_status, statusMsg.c_str());
      
      // Suscribirse a topics si es necesario (opcional)
      // mqttClient.subscribe("aquatech/commands");
    } else {
      mqttConnected = false;
      Serial.print("❌ Error MQTT, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" (reintentando en 5 segundos)");
    }
  }
}

void publishToMQTT() {
  if (!mqttClient.connected()) {
    return;
  }
  
  // Publicar presión IN
  String payload_in = String(presion_in, 1);
  if (mqttClient.publish(mqtt_topic_presion_in, payload_in.c_str())) {
    Serial.print("[MQTT] Publicado presion_in: ");
    Serial.println(payload_in);
  } else {
    Serial.println("[MQTT] Error al publicar presion_in");
  }
  
  // Publicar presión OUT
  String payload_out = String(presion_out, 1);
  if (mqttClient.publish(mqtt_topic_presion_out, payload_out.c_str())) {
    Serial.print("[MQTT] Publicado presion_out: ");
    Serial.println(payload_out);
  } else {
    Serial.println("[MQTT] Error al publicar presion_out");
  }
  
  // Publicar mensaje combinado con timestamp (opcional)
  unsigned long seconds = millis() / 1000;
  String combinedMsg = "{\"presion_in\":" + String(presion_in, 1) + 
                       ",\"presion_out\":" + String(presion_out, 1) + 
                       ",\"timestamp\":" + String(seconds) + 
                       ",\"source\":\"" + dataSource + "\"}";
  mqttClient.publish("aquatech/data", combinedMsg.c_str());
}
