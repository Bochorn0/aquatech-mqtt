#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// WiFi
const char* ssid = "BochoLandStarlink2.4";
const char* password = "Sakaunstarlink24*";

// API
const char* loginUrl = "http://192.168.1.31:3009/api/v1.0/auth/login";
const char* dataUrl = "http://192.168.1.31:3009/api/v1.0/products/componentInput";

// Pines a leer
const int pinList[] = {0, 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23,
                       25, 26, 27, 32, 33, 34, 35, 36, 39};
const int numPins = sizeof(pinList) / sizeof(pinList[0]);

String jwtToken = ""; // Guardará el token

void setup() {
  Serial.begin(115200);

  // Configurar pines
  for (int i = 0; i < numPins; i++) {
    pinMode(pinList[i], INPUT_PULLUP);
  }

  // Conectar a WiFi
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado a WiFi");

  // Hacer login
  loginToApi();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && jwtToken != "") {
    sendDataToApi();
  } else {
    Serial.println("No conectado o sin token. Reintentando login...");
    loginToApi();
  }

  delay(10000); // Espera 10s antes del siguiente envío
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

    // Parsear token con ArduinoJson
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
  HTTPClient http;
  http.begin(dataUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + jwtToken);

  // Armar el JSON manualmente
  String jsonData = "{";

  // Sección de datos simulados
  jsonData += "\"mock_data\":{";
  jsonData += "\"pressure\":12.34,\"flow\":7.89,\"tds\":300,\"temperature\":80";
  jsonData += "},";

  // Sección de pines
  jsonData += "\"pin_status\":{";
  for (int i = 0; i < numPins; i++) {
    int val = digitalRead(pinList[i]);
    jsonData += "\"gpio" + String(pinList[i]) + "\":" + String(val);
    if (i < numPins - 1) jsonData += ",";
  }
  jsonData += "}";

  jsonData += "}";

  int httpCode = http.POST(jsonData);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("Datos enviados correctamente:");
    Serial.println(response);
  } else {
    Serial.print("Fallo al enviar datos, código: ");
    Serial.println(httpCode);

    // Si hubo error 401, probablemente expiró el token → re-login
    if (httpCode == 401) {
      Serial.println("Token expirado, iniciando nuevo login...");
      loginToApi();
    }
  }

  http.end();
}
