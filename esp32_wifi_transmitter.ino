// ================== ESP32 WiFi Transmitter ==================
// ESP32 WROOM32 común que envía datos de presión vía WiFi UDP
// NO necesita módulo LoRa, solo WiFi (que todos los ESP32 tienen)
// Envía: presion_in y presion_out con valores aleatorios

#include <WiFi.h>
#include <WiFiUdp.h>

// ================== WiFi Configuration ==================
const char* ssid = "BochoLandStarlink2.4";
const char* password = "Sakaunstarlink24*";

// ================== UDP Configuration ==================
WiFiUDP udp;
const char* udpAddress = "192.168.1.255";  // Broadcast address (envía a todos en la red)
// O usa la IP específica del receptor: "192.168.1.100"
const int udpPort = 12345;  // Puerto UDP

// ================== Timing ==================
unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 2000; // Enviar cada 2 segundos

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==========================================");
  Serial.println("ESP32 WiFi Transmitter");
  Serial.println("Enviando datos de presión vía WiFi UDP");
  Serial.println("==========================================\n");
  
  // Conectar a WiFi
  Serial.print("Conectando a WiFi: ");
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
    Serial.println("✅ WiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    
    // Inicializar UDP
    udp.begin(udpPort);
    Serial.print("UDP iniciado en puerto ");
    Serial.println(udpPort);
    Serial.print("Enviando a: ");
    Serial.print(udpAddress);
    Serial.print(":");
    Serial.println(udpPort);
    Serial.println("\nEnviando datos cada 2 segundos...\n");
  } else {
    Serial.println("❌ Error: No se pudo conectar a WiFi");
    Serial.println("Verifica las credenciales");
    while (1) delay(1000); // Detener si no hay WiFi
  }
}

// ================== Loop ==================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado, intentando reconectar...");
    WiFi.reconnect();
    delay(2000);
    return;
  }
  
  if (millis() - lastSend >= SEND_INTERVAL) {
    // Generar valores aleatorios para presión
    float presion_in = random(0, 1000) / 10.0;   // 0.0 a 99.9
    float presion_out = random(0, 1000) / 10.0;  // 0.0 a 99.9
    
    // Crear mensaje
    String message = "presion_in:" + String(presion_in, 1) + ",presion_out:" + String(presion_out, 1);
    
    // Enviar paquete UDP
    udp.beginPacket(udpAddress, udpPort);
    udp.print(message);
    udp.endPacket();
    
    // Mostrar en Serial
    Serial.print("[TX] Enviado: ");
    Serial.print(message);
    Serial.print(" | IP: ");
    Serial.print(WiFi.localIP());
    Serial.print(" | RSSI: ");
    Serial.println(WiFi.RSSI());
    
    lastSend = millis();
  }
  
  delay(100);
}





