// ================== ESP32 OLED WiFi & LoRa Monitor ==================
// Sketch para mostrar estado WiFi y escanear dispositivos LoRa

#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LoRa.h>
// Librería PubSubClient para MQTT
// Instalar desde: Sketch → Include Library → Manage Libraries → Buscar "PubSubClient" (Nick O'Leary)
// O desde: https://github.com/knolleary/pubsubclient
#include <PubSubClient.h>

// ================== WiFi Configuration ==================
const char* ssid = "Oneplus";
const char* password = "";  // Sin contraseña

// ================== UDP Configuration ==================
WiFiUDP udp;
const int udpPort = 12345;  // Puerto UDP para recibir datos

// ================== MQTT Configuration ==================
const char* mqtt_server = "146.190.143.141";
const int mqtt_port_open = 1883;        // Puerto sin autenticación
const int mqtt_port_secure = 8883;      // Puerto con autenticación y TLS
const char* mqtt_client_id = "ESP32_LoRa_Receiver";

// Credenciales MQTT para puerto seguro
const char* mqtt_username = "Aquatech001";
const char* mqtt_password = "Aquatech2025*";

// ================== Certificado CA para TLS ==================
// Certificado CA del servidor Mosquitto (Aquatech)
const char* ca_cert = \
"-----BEGIN CERTIFICATE-----\n" \
"MIIECTCCAvGgAwIBAgIUaeT7mWBE0krpOQdDiG/akjnNe9MwDQYJKoZIhvcNAQEL\n" \
"BQAwgZMxCzAJBgNVBAYTAk1YMQ8wDQYDVQQIDAZTb25vcmExEzARBgNVBAcMCkhl\n" \
"cm1vc2lsbG8xETAPBgNVBAoMCEFxdWF0ZWNoMQswCQYDVQQLDAJUSTETMBEGA1UE\n" \
"AwwKQXF1YXRlY2hUSTEpMCcGCSqGSIb3DQEJARYaYXF1YXRlY2guaXQuMjAyNUBn\n" \
"bWFpbC5jb20wHhcNMjUxMjI2MTQwMzE4WhcNMzUxMjI0MTQwMzE4WjCBkzELMAkG\n" \
"A1UEBhMCTVgxDzANBgNVBAgMBlNvbm9yYTETMBEGA1UEBwwKSGVybW9zaWxsbzER\n" \
"MA8GA1UECgwIQXF1YXRlY2gxCzAJBgNVBAsMAlRJMRMwEQYDVQQDDApBcXVhdGVj\n" \
"aFRJMSkwJwYJKoZIhvcNAQkBFhphcXVhdGVjaC5pdC4yMDI1QGdtYWlsLmNvbTCC\n" \
"ASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKY2bVSij845H6cMaX3CHHdu\n" \
"zN/EAa5bYHCt8Y5ACphCzLmS5BwBCG0MTgKBLckYaH3qdzjEvMt+jeZ37N2f/Kmh\n" \
"DDj1LXeSzXAG/tKeNt/dp2FgsF2mblCRaYZxwyBpZjaa/pv30kahNmeiU1euLoBi\n" \
"BaKOKgyXbSvU7AJ3trT09ZDWUIzicoEw7zr4zPe4eL/0A7yE03JSNNrsb06QJjcz\n" \
"JIJUeg15GlzIi2hWmYYg/rX11znYq94CUNEf6wbbwZmh7oaEYwO/ru9nq0JaCzDs\n" \
"lqpKEkSo4VedfamD2zE7v8ncD+SSWzR/gSI+dJejAxsJ3HCVCeUzA1IOsVqkZG0C\n" \
"AwEAAaNTMFEwHQYDVR0OBBYEFMJCox/DWSVVUDcl0+AOZyxGkMy8MB8GA1UdIwQY\n" \
"MBaAFMJCox/DWSVVUDcl0+AOZyxGkMy8MA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZI\n" \
"hvcNAQELBQADggEBAJ2q5IZdQSg1lLG2nKu9/HY2QUVf2lsi2lD+x9bA1DX6rw5+\n" \
"s8Fz+ytZKrsDEciVcYgs9BhEVmP8AnZPcaE9pimJXqSBK8tehh/ZJtUZv2Vvp5g/\n" \
"K6EvShFcvHqXsXQW8nhPvESRaE7bucSCONNS8Cuy/BDQ+ffE6USWzeVY4YwYcJ4g\n" \
"C0l3buWSVNfbwL5HHTupUze06pn9zZgJbfcFk+WlwNwIizK3DPg39bom/0HT8+Fz\n" \
"BYZgMEvHi/6B83pecj+MoAVPhpwl8549NE92Sszv8OIKpR59WOuC+a4NiVktCctS\n" \
"U0YBXM/WsHxY/PyQl3qShJMZT3Q65aQAnC2Wocg=\n" \
"-----END CERTIFICATE-----\n";

// Opción: Si quieres desactivar la verificación del certificado (solo desarrollo)
// Cambia USE_TLS_VERIFY a false
const bool USE_TLS_VERIFY = true;  // true = verificar certificado, false = setInsecure()

// Gateway ID único - CAMBIAR ESTE VALOR para cada gateway
// Debe coincidir con el campo "id" del Controller en la base de datos
const char* gateway_id = "gateway001";  // ⚠️ CAMBIAR según tu gateway

// Topics MQTT con nuevo formato: aquatech/gateway/{gateway_id}/...
String mqtt_topic_data = "aquatech/gateway/" + String(gateway_id) + "/data";
String mqtt_topic_status = "aquatech/gateway/" + String(gateway_id) + "/status";

// Clientes MQTT (dos instancias: una para cada puerto)
WiFiClient espClient;                        // Para puerto 1883 (sin auth)
WiFiClientSecure espClientSecure;           // Para puerto 8883 (con auth y TLS)
PubSubClient mqttClientOpen(espClient);     // Puerto 1883 (sin auth)
PubSubClient mqttClientSecure(espClientSecure);  // Puerto 8883 (con auth y TLS)

bool mqttOpenConnected = false;
bool mqttSecureConnected = false;

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
bool mqttConnected = false;  // Mantener para compatibilidad con display
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 500; // Actualizar cada 500ms
unsigned long lastMqttReconnect = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000; // Intentar reconectar cada 5 segundos

// Datos de presión (generados aleatoriamente para pruebas)
float presion_in = 0.0;
float presion_out = 0.0;
unsigned long lastDataReceived = 0;
int packetCount = 0;
String dataSource = "Test"; // "Test" - datos generados automáticamente

// Temporizador para generar y enviar datos de prueba
unsigned long lastTestDataSent = 0;
const unsigned long TEST_DATA_INTERVAL = 5000; // Enviar datos cada 5 segundos

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==========================================");
  Serial.println("ESP32 WiFi & LoRa Monitor");
  Serial.println("==========================================\n");
  
  // Detectar y inicializar OLED
  Serial.println("\n[Display] Intentando inicializar OLED...");
  if (initOLED()) {
    displayInitialized = true;
    Serial.println("✅ Display inicializado correctamente!");
    Serial.print("  SDA: ");
    Serial.println(OLED_SDA);
    Serial.print("  SCL: ");
    Serial.println(OLED_SCL);
    Serial.print("  Dirección I2C: 0x");
    Serial.println(OLED_ADDR, HEX);
    
    // Mostrar mensaje inicial
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("ESP32 LoRa");
    display.println("SX1276 915MHz");
    display.println("Inicializando...");
    display.setCursor(0, 10);
    display.println("Display OK");
    display.setCursor(0, 20);
    display.print("SDA:");
    display.print(OLED_SDA);
    display.print(" SCL:");
    display.print(OLED_SCL);
    display.display();
    delay(2000);
  } else {
    Serial.println("❌ Error: No se pudo inicializar el display");
    Serial.println("⚠️  Continuando sin display...");
    Serial.println("   Verifica:");
    Serial.println("   - Conexiones SDA/SCL");
    Serial.println("   - Alimentación del display");
    Serial.println("   - Dirección I2C (0x3C o 0x3D)");
    displayInitialized = false;
  }
  
  // Inicializar LoRa (desactivado para pruebas - solo datos random)
  loraInitialized = false;
  Serial.println("\n⚠️  LoRa desactivado - usando datos de prueba aleatorios");
  
  // Conectar a WiFi
  Serial.println("\nConectando a WiFi...");
  connectToWiFi();
  
  // Inicializar MQTT
  if (wifiConnected) {
    Serial.println("\nInicializando MQTT...");
    Serial.println("  Puerto 1883: Sin autenticación");
    Serial.println("  Puerto 8883: Con autenticación (usuario: Aquatech001)");
    initMQTT();
  }
  
  // Dibujar display completo inicial
  if (displayInitialized) {
    drawCompleteDisplay();
  }
  
  Serial.println("\n✅ Sistema listo.\n");
  if (!displayInitialized) {
    Serial.println("⚠️  NOTA: Sistema funcionando sin display");
    Serial.println("   El Serial Monitor mostrará toda la información");
  }
}

// ================== Loop ==================
void loop() {
  // Continuar funcionando aunque el display no esté inicializado
  // Solo actualizar display si está inicializado
  
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
  
  // Escanear dispositivos LoRa si está inicializado (desactivado para pruebas)
  // if (loraInitialized) {
  //   scanLoRaDevices();
  // }
  
  // Recibir datos WiFi UDP si está conectado (desactivado para pruebas)
  // if (wifiConnected) {
  //   receiveWiFiData();
  // }
  
  // Generar y enviar datos de presión aleatorios periódicamente
  if (wifiConnected && mqttSecureConnected) {
    if (millis() - lastTestDataSent >= TEST_DATA_INTERVAL) {
      generateRandomPressureData();
      publishToMQTT();
      lastTestDataSent = millis();
    }
  }
  
  // Manejar MQTT (solo puerto 8883 activo para publicación)
  if (wifiConnected) {
    reconnectMQTT();
    // mqttClientOpen.loop();      // Puerto 1883 desactivado - no se publica
    mqttClientSecure.loop();   // Mantener conexión MQTT 8883 activa
    
    // Actualizar estado mqttConnected para display (solo puerto 8883)
    mqttConnected = mqttSecureConnected;  // Solo considerar puerto 8883
  }
  
  // Actualizar display solo si está inicializado
  if (displayInitialized && millis() - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    updateDisplay();
    lastDisplayUpdate = millis();
  }
  
  // Intentar reinicializar display si no está inicializado (cada 10 segundos)
  static unsigned long lastDisplayRetry = 0;
  if (!displayInitialized && millis() - lastDisplayRetry >= 10000) {
    Serial.println("[Display] Reintentando inicializar display...");
    if (initOLED()) {
      displayInitialized = true;
      Serial.println("✅ Display inicializado!");
      drawCompleteDisplay();
    }
    lastDisplayRetry = millis();
  }
  
  delay(100);
}

// ================== Display Functions ==================
void drawCompleteDisplay() {
  if (!displayInitialized) return;
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  
  // Título
  display.setCursor(0, 0);
  display.println("ESP32 LoRa Gateway");
  
  // Línea separadora
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  
  updateDisplay();
}

void updateDisplay() {
  if (!displayInitialized) return;
  
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
      Serial.print("[Display] Probando SDA=");
      Serial.print(configs[i].sda);
      Serial.print(", SCL=");
      Serial.print(configs[i].scl);
      Serial.print(", RST=");
      Serial.print(configs[i].rst);
      Serial.print(", Addr=0x");
      Serial.print(addresses[j], HEX);
      Serial.print("... ");
      
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
        
        Serial.println("✅ OK!");
        Serial.print("[Display] ✅ Display encontrado! SDA=");
        Serial.print(OLED_SDA);
        Serial.print(", SCL=");
        Serial.print(OLED_SCL);
        Serial.print(", Addr=0x");
        Serial.println(OLED_ADDR, HEX);
        
        return true;
      } else {
        Serial.println("❌ Fallo");
      }
      delay(100);
    }
  }
  
  Serial.println("[Display] ❌ No se encontró ningún display compatible");
  Serial.println("[Display] Configuraciones probadas:");
  Serial.println("  - TTGO LoRa32: SDA=4, SCL=15, RST=16");
  Serial.println("  - Estándar ESP32: SDA=21, SCL=22");
  Serial.println("  - Con RST: SDA=21, SCL=22, RST=16");
  Serial.println("[Display] Direcciones I2C probadas: 0x3C, 0x3D");
  
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
    
    // Inicializar UDP para recibir datos (desactivado para pruebas)
    // udp.begin(udpPort);
    // Serial.print("UDP iniciado en puerto ");
    // Serial.println(udpPort);
    
    Serial.println("✅ Sistema listo - generando datos de prueba aleatorios");
  } else {
    wifiConnected = false;
    Serial.println("❌ Error: No se pudo conectar a WiFi");
  }
}

// ================== Test Data Generation ==================
void generateRandomPressureData() {
  // Generar valores aleatorios de presión
  // Presión IN: entre 30-100 PSI
  presion_in = 30.0 + (random(0, 700) / 10.0); // 30.0 a 100.0 PSI
  
  // Presión OUT: entre 50-120 PSI (siempre mayor que IN)
  presion_out = presion_in + 20.0 + (random(0, 500) / 10.0); // IN+20 a IN+70
  if (presion_out > 120.0) presion_out = 120.0;
  
  packetCount++;
  lastDataReceived = millis();
  dataSource = "Test";
  
  Serial.print("[Test] Datos generados #");
  Serial.print(packetCount);
  Serial.print(" | Presion IN: ");
  Serial.print(presion_in, 1);
  Serial.print(" PSI | Presion OUT: ");
  Serial.print(presion_out, 1);
  Serial.println(" PSI");
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
  
  // Publicar a MQTT solo si está conectado al puerto 8883
  // Puerto 1883 desactivado - solo se publica en puerto 8883
  if (mqttSecureConnected) {
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
  // Configurar cliente para puerto 1883 (sin autenticación)
  mqttClientOpen.setServer(mqtt_server, mqtt_port_open);
  mqttClientOpen.setCallback(mqttCallback);
  mqttClientOpen.setSocketTimeout(5);  // Timeout de 5 segundos
  
  // Configurar cliente para puerto 8883 (con autenticación y TLS)
  espClientSecure.setTimeout(10); // Timeout de 10 segundos
  
  // Configurar certificado CA para TLS
  if (USE_TLS_VERIFY) {
    // Usar certificado CA para verificación
    espClientSecure.setCACert(ca_cert);
    Serial.println("[MQTT-8883] ✅ Certificado CA configurado (verificación TLS activa)");
  } else {
    // Desactivar verificación (solo para desarrollo)
    espClientSecure.setInsecure();
    Serial.println("[MQTT-8883] ⚠️  Verificación TLS desactivada (solo desarrollo)");
  }
  
  mqttClientSecure.setServer(mqtt_server, mqtt_port_secure);
  mqttClientSecure.setCallback(mqttCallback);
  mqttClientSecure.setSocketTimeout(10);
  
  Serial.println("[MQTT] Configuración:");
  Serial.print("  Puerto 1883: ");
  Serial.print(mqtt_server);
  Serial.print(":");
  Serial.print(mqtt_port_open);
  Serial.println(" (sin autenticación)");
  Serial.print("  Puerto 8883: ");
  Serial.print(mqtt_server);
  Serial.print(":");
  Serial.print(mqtt_port_secure);
  Serial.println(" (con autenticación y TLS)");
  Serial.print("  Usuario: ");
  Serial.println(mqtt_username);
  Serial.print("  TLS: ");
  Serial.println(USE_TLS_VERIFY ? "Verificación activa" : "Verificación desactivada");
  
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
  
  if (!wifiConnected) {
    return;
  }
  
  // ====== CONECTAR AL PUERTO 1883 (SIN AUTENTICACIÓN) - DESACTIVADO ======
  // Puerto 1883 desactivado - solo se usa puerto 8883
  /*
  if (!mqttClientOpen.connected()) {
    Serial.print("[MQTT-1883] Conectando...");
    
    if (mqttClientOpen.connect(mqtt_client_id)) {
      mqttOpenConnected = true;
      Serial.println(" ✅ Conectado!");
      
      // Publicar mensaje de estado
      String statusMsg = "{\"status\":\"online\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"port\":1883}";
      mqttClientOpen.publish(mqtt_topic_status.c_str(), statusMsg.c_str());
    } else {
      mqttOpenConnected = false;
      Serial.print(" ❌ Error, rc=");
      Serial.println(mqttClientOpen.state());
    }
  }
  */
  
  // ====== CONECTAR AL PUERTO 8883 (CON AUTENTICACIÓN Y TLS) ======
  if (!mqttClientSecure.connected()) {
    Serial.print("[MQTT-8883] Conectando con usuario: ");
    Serial.print(mqtt_username);
    Serial.print(" (TLS)...");
    
    // Conectar con autenticación y TLS
    if (mqttClientSecure.connect(mqtt_client_id, mqtt_username, mqtt_password)) {
      mqttSecureConnected = true;
      Serial.println(" ✅ Conectado!");
      
      // Publicar mensaje de estado
      String statusMsg = "{\"status\":\"online\",\"ip\":\"" + WiFi.localIP().toString() + "\",\"port\":8883}";
      mqttClientSecure.publish(mqtt_topic_status.c_str(), statusMsg.c_str());
    } else {
      mqttSecureConnected = false;
      Serial.print(" ❌ Error, rc=");
      int state = mqttClientSecure.state();
      Serial.print(state);
      
      // Mensajes de error más descriptivos
      switch (state) {
        case -4:
          Serial.println(" - Timeout (verificar que el puerto 8883 está abierto)");
          break;
        case -3:
          Serial.println(" - Conexión perdida");
          break;
        case -2:
          Serial.println(" - Fallo de conexión TLS");
          Serial.println("   Verificar:");
          Serial.println("   - Puerto 8883 está abierto en el servidor");
          Serial.println("   - TLS está configurado correctamente en Mosquitto");
          Serial.println("   - Certificado CA es correcto y está en el código");
          Serial.println("   - Firewall permite conexiones al puerto 8883");
          Serial.println("   - Credenciales correctas (usuario: Aquatech001)");
          if (!USE_TLS_VERIFY) {
            Serial.println("   ⚠️  Verificación TLS desactivada - considerar activarla");
          }
          Serial.println("   - Probar desde servidor:");
          Serial.println("     mosquitto_sub -h 146.190.143.141 -p 8883 --cafile /etc/mosquitto/certs/ca.crt -u Aquatech001 -P 'Aquatech2025*' -t 'test'");
          break;
        case -1:
          Serial.println(" - Desconectado");
          break;
        case 4:
          Serial.println(" - Credenciales incorrectas");
          break;
        case 5:
          Serial.println(" - No autorizado (verificar ACL)");
          break;
        default:
          Serial.print(" - Error desconocido (código: ");
          Serial.print(state);
          Serial.println(")");
          break;
      }
    }
  }
}

void publishToMQTT() {
  // Crear mensaje JSON con nuevo formato
  // Formato: { gateway_id, timestamp, sensors: { pressure_in, pressure_out, water_level }, source, rssi }
  unsigned long seconds = millis() / 1000;
  
  // Construir JSON manualmente
  String jsonMsg = "{";
  jsonMsg += "\"gateway_id\":\"" + String(gateway_id) + "\",";
  jsonMsg += "\"timestamp\":" + String(seconds) + ",";
  jsonMsg += "\"sensors\":{";
  jsonMsg += "\"pressure_in\":" + String(presion_in, 1) + ",";
  jsonMsg += "\"pressure_out\":" + String(presion_out, 1);
  // Agregar water_level cuando esté disponible
  // jsonMsg += ",\"water_level\":" + String(water_level, 1);
  jsonMsg += "},";
  jsonMsg += "\"source\":\"" + dataSource + "\"";
  // Agregar RSSI si está disponible
  // jsonMsg += ",\"rssi\":" + String(loraRssi);
  jsonMsg += "}";
  
  // Publicar en puerto 1883 (sin autenticación) - DESACTIVADO
  // Solo se publica en puerto 8883 (con autenticación y TLS)
  /*
  if (mqttOpenConnected && mqttClientOpen.connected()) {
    if (mqttClientOpen.publish(mqtt_topic_data.c_str(), jsonMsg.c_str())) {
      // Serial.print("[MQTT-1883] ✅ Publicado: ");
      // Serial.println(jsonMsg);
    } else {
      Serial.println("[MQTT-1883] ❌ Error al publicar");
      mqttOpenConnected = false;
    }
  }
  */
  
  // Publicar en puerto 8883 (con autenticación y TLS) - ACTIVO
  if (mqttSecureConnected && mqttClientSecure.connected()) {
    if (mqttClientSecure.publish(mqtt_topic_data.c_str(), jsonMsg.c_str())) {
      // Serial.print("[MQTT-8883] ✅ Publicado: ");
      // Serial.println(jsonMsg);
    } else {
      Serial.println("[MQTT-8883] ❌ Error al publicar");
      mqttSecureConnected = false;
    }
  }
}
