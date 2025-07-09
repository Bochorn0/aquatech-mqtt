#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

// WiFi
const char* ssid = "OnePlus 8t";
const char* password = "";
//const char* ip = "192.168.1.31";
const char* ip = "164.92.95.176"; // prod
const char* productId = "67d22123befc47e74b7ef262"; // <-- reemplaza por tu ObjectId real


char loginUrl[100];
char dataUrl[100];


// Pines digitales para estado
const int pinList[] = {5, 13, 14, 16, 17, 21, 22, 25, 26, 27, 32, 33, 34, 35, 36, 39};
const int numPins = sizeof(pinList) / sizeof(pinList[0]);

const int TDS_PIN = 32;
const byte flujoPin1 = 15;
const byte flujoPin2 = 4;

volatile int pulsosFlujo1 = 0;
volatile int pulsosFlujo2 = 0;
float flujo1_Lmin = 0.0;
float flujo2_Lmin = 0.0;

String jwtToken = "";
float bluetoothTemperature = 0.0;
unsigned long tiempoPrevio = 0;

void IRAM_ATTR contarFlujo1() {
  pulsosFlujo1++;
}
void IRAM_ATTR contarFlujo2() {
  pulsosFlujo2++;
}

void setup() {
  // Formar URLs sin usar String
  snprintf(loginUrl, sizeof(loginUrl), "http://%s:3009/api/v1.0/auth/login", ip);
  snprintf(dataUrl, sizeof(dataUrl), "http://%s:3009/api/v1.0/products/componentInput", ip);

  Serial.begin(115200);
  SerialBT.begin("ESP32_BT_Device");
  Serial.println("Bluetooth iniciado. Esperando datos de temperatura...");

  analogSetAttenuation(ADC_11db);
  for (int i = 0; i < numPins; i++) {
    pinMode(pinList[i], INPUT_PULLUP);
  }

  pinMode(flujoPin1, INPUT_PULLUP);
  pinMode(flujoPin2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(flujoPin1), contarFlujo1, RISING);
  attachInterrupt(digitalPinToInterrupt(flujoPin2), contarFlujo2, RISING);

  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado a WiFi");

  loginToApi();
}

void loop() {
  if (SerialBT.available()) {
    String btInput = SerialBT.readStringUntil('\n');
    btInput.trim();
    float temp = btInput.toFloat();
    if (temp > 0 && temp < 100) {
      bluetoothTemperature = temp;
    }
  }
  if (millis() - tiempoPrevio >= 1000) {
    detachInterrupt(digitalPinToInterrupt(flujoPin1));
    detachInterrupt(digitalPinToInterrupt(flujoPin2));

    flujo1_Lmin = pulsosFlujo1 / 5.5;
    flujo2_Lmin = pulsosFlujo2 / 5.5;
    pulsosFlujo1 = 0;
    pulsosFlujo2 = 0;

    tiempoPrevio = millis();

    attachInterrupt(digitalPinToInterrupt(flujoPin1), contarFlujo1, RISING);
    attachInterrupt(digitalPinToInterrupt(flujoPin2), contarFlujo2, RISING);

    // Solo enviar si hay flujo
    if (flujo1_Lmin > 0 || flujo2_Lmin > 0) {
      if (WiFi.status() == WL_CONNECTED && jwtToken != "") {
        sendDataToApi();
      } else {
        loginToApi();
      }
    } else {
      Serial.println("Flujo = 0 en ambos sensores. No se envía.");
    }
  }
}

void loginToApi() {
  HTTPClient http;
  http.begin(loginUrl);
  http.addHeader("Content-Type", "application/json");

  String loginPayload = "{\"email\":\"esp32@lcc.com.mx\",\"password\":\"Esp32*\"}";
  int httpCode = http.POST(loginPayload);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("Login exitoso:");
    Serial.println(response);

    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, response);
    if (!error && doc.containsKey("token")) {
      jwtToken = doc["token"].as<String>();
    } else {
      Serial.println("Error al parsear token");
      jwtToken = "";
    }
  } else {
    Serial.print("Fallo en login, código: ");
    Serial.println(httpCode);
    jwtToken = "";
  }

  http.end();
}

void sendDataToApi() {
  int tdsRaw = analogRead(TDS_PIN);
  float tdsVoltage = (tdsRaw / 4095.0) * 3.3;

  Serial.println("Enviando datos a API...");

  DynamicJsonDocument doc(2048);
    // Agregar el ID del producto
  doc["producto"] = productId;
  JsonObject real_data = doc.createNestedObject("real_data");
  real_data["tds"] = tdsVoltage;
  real_data["temperature"] = bluetoothTemperature;
  real_data["flujo_bomba"] = flujo1_Lmin;
  real_data["flujo_rechazo"] = flujo2_Lmin;

  JsonObject pin_status = doc.createNestedObject("pin_status");
  for (int i = 0; i < numPins; i++) {
    pin_status["gpio" + String(pinList[i])] = digitalRead(pinList[i]);
  }

  String jsonData;
  serializeJson(doc, jsonData);

  HTTPClient http;
  http.begin(dataUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + jwtToken);

  int httpCode = http.POST(jsonData);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("Datos enviados correctamente:");
    Serial.println(response);
  } else {
    Serial.print("Error al enviar datos: ");
    Serial.println(httpCode);
    if (httpCode == 401) {
      Serial.println("Token expirado. Reintentando login...");
      loginToApi();
    }
  }

  http.end();
}
