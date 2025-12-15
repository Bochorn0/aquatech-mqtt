// ================== ESP32 LoRa Transmitter ==================
// ESP32 WROOM32 que envía datos de presión vía LoRa
// Envía: presion_in y presion_out con valores aleatorios

#include <SPI.h>
#include <LoRa.h>

// ================== LoRa Configuration ==================
// Pines para SX1276 ESP32 LoRa 915 MHz
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_CS 18
#define LORA_RST 14
#define LORA_DIO0 26
#define LORA_FREQUENCY 915E6  // 915 MHz

// ================== Timing ==================
unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL = 2000; // Enviar cada 2 segundos

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n==========================================");
  Serial.println("ESP32 LoRa Transmitter");
  Serial.println("Enviando datos de presión");
  Serial.println("==========================================\n");
  
  // Inicializar SPI para LoRa
  Serial.println("Inicializando SPI...");
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  delay(100);
  
  // Inicializar LoRa
  Serial.println("Inicializando LoRa...");
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("❌ Error al inicializar LoRa!");
    Serial.println("Verifica las conexiones SPI");
    while (1) delay(1000); // Detener si no se puede inicializar
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
  Serial.println("\nEnviando datos cada 2 segundos...\n");
}

// ================== Loop ==================
void loop() {
  if (millis() - lastSend >= SEND_INTERVAL) {
    // Generar valores aleatorios para presión
    float presion_in = random(0, 1000) / 10.0;   // 0.0 a 99.9
    float presion_out = random(0, 1000) / 10.0;  // 0.0 a 99.9
    
    // Crear mensaje JSON simple
    String message = "presion_in:" + String(presion_in, 1) + ",presion_out:" + String(presion_out, 1);
    
    // Enviar paquete LoRa
    LoRa.beginPacket();
    LoRa.print(message);
    LoRa.endPacket();
    
    // Mostrar en Serial
    Serial.print("[TX] Enviado: ");
    Serial.print(message);
    Serial.print(" | RSSI: ");
    Serial.println(LoRa.packetRssi());
    
    lastSend = millis();
  }
  
  delay(100);
}

