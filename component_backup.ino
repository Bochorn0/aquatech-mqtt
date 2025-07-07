#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

// WiFi
const char* ssid = "OnePlus 8t";
const char* password = "";

// API
const char* loginUrl = "http://192.168.7.149:3009/api/v1.0/auth/login";
const char* dataUrl = "http://192.168.7.149:3009/api/v1.0/products/componentInput";

// Pines digitales seguros
const int pinList[] = {5, 13, 14, 16, 17, 21, 22, 25, 26, 27, 32, 33, 34, 35, 36, 39};
const int numPins = sizeof(pinList) / sizeof(pinList[0]);

const int TDS_PIN = 32;

String jwtToken = "";
float bluetoothTemperature = 0.0;

void setup() {
  Serial.begin(115200);
  SerialBT.begin("ESP32_BT_Device");
  Serial.println("Bluetooth iniciado. Esperando datos de temperatura...");

  analogSetAttenuation(ADC_11db);

  for (int i = 0; i < numPins; i++) {
    pinMode(pinList[i], INPUT_PULLUP);
  }

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
    Serial.print("Temperatura recibida por Bluetooth: ");
    Serial.println(btInput);

    // Convertir a número flotante si es válido
    float temp = btInput.toFloat();
    if (temp > 0 && temp < 100) {
      bluetoothTemperature = temp;
    } else {
      Serial.println("Temperatura no válida. Ignorada.");
    }
  }

  if (WiFi.status() == WL_CONNECTED && jwtToken != "") {
    sendDataToApi();
  } else {
    Serial.println("No conectado o sin token. Reintentando login...");
    loginToApi();
  }

  delay(10000);
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

  Serial.print("Lectura TDS raw: ");
  Serial.print(tdsRaw);
  Serial.print(" - Voltaje: ");
  Serial.println(tdsVoltage, 3);

  Serial.print("Temperatura (Bluetooth): ");
  Serial.println(bluetoothTemperature, 2);

  String jsonData = "{";
  jsonData += "\"real_data\":{";
  jsonData += "\"tds\":" + String(tdsVoltage, 3) + ",";
  jsonData += "\"temperature\":" + String(bluetoothTemperature, 2);
  jsonData += "},";

  jsonData += "\"pin_status\":{";
  for (int i = 0; i < numPins; i++) {
    int val = digitalRead(pinList[i]);
    jsonData += "\"gpio" + String(pinList[i]) + "\":" + String(val);
    if (i < numPins - 1) jsonData += ",";
  }
  jsonData += "}";
  jsonData += "}";

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
    Serial.print("Fallo al enviar datos, código: ");
    Serial.println(httpCode);
    if (httpCode == 401) {
      Serial.println("Token expirado, iniciando nuevo login...");
      loginToApi();
    }
  }

  http.end();
}

