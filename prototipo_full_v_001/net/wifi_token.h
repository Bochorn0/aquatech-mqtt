#pragma once

// El WiFi solo consulta. Despues se apaga para poder leer GPIO 12 y 13.
// 0 local, 1 Bluetooth, 2 conectando, 3 WiFi, 4 token listo y radio apagada.
// netRadioOn es true solo mientras la radio esta encendida.

#include <BluetoothSerial.h>
#include <time.h>

#include "wifi_manager.h"
#include "auth_manager.h"
#include "config_sync.h"

static volatile int netPhase = 0;
static volatile bool netRadioOn = false;
static volatile bool netHoldUi = false;
static volatile bool netOverlayReady = false;
static volatile bool netNeedsRedraw = false;
static volatile unsigned long netUpdatedAt = 0;
static volatile unsigned long netNextAt = 0;
static volatile time_t netUpdatedClock = 0;
static bool tokenLogged = false;
static bool tokenProvision = false;
static unsigned long netSyncMs = 60000;
static const char* tokenHost = "";
static const char* tokenEmail = "";
static const char* tokenPassword = "";
static BluetoothSerial netBt;
static bool netBtOn = false;

static void netBtStart(const char* name) {
  if (netBtOn) return;
  if (!netBt.begin(name)) {
    Serial.println("[NET] Bluetooth no abrio. Usa el cable.");
    return;
  }
  netBtOn = true;
  netPhase = 1;
  Serial.printf("[NET] Bluetooth %s\n", name);
}

static void netBtStop() {
  if (!netBtOn) return;
  netBt.end();
  netBtOn = false;
  Serial.println("[NET] Bluetooth apagado");
}

static void tokenHandleLine(String line) {
  line.trim();
  if (!line.length()) return;
  if (line == "WIFI_RESET") {
    wifiClear();
    tokenLogged = false;
    tokenProvision = true;
    netPhase = 1;
    Serial.println("[NET] WiFi borrado. Envia NombreRed,clave");
    return;
  }
  int comma = line.indexOf(',');
  if (comma <= 0) return;
  String ssid = line.substring(0, comma);
  String pass = line.substring(comma + 1);
  ssid.trim();
  pass.trim();
  if (!ssid.length() || ssid.length() > 32) return;
  wifiSave(ssid.c_str(), pass.c_str());
  tokenLogged = false;
  tokenProvision = false;
  Serial.printf("[NET] red %s\n", wifiSsid);
}

static void tokenReadStream(Stream& stream, char* line, int& used) {
  while (stream.available()) {
    char c = (char)stream.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[used] = 0;
      used = 0;
      tokenHandleLine(String(line));
      continue;
    }
    if (used < 95) line[used++] = c;
  }
}

static void tokenPumpLines() {
  static char serialLine[96];
  static int serialUsed = 0;
  static char btLine[96];
  static int btUsed = 0;
  tokenReadStream(Serial, serialLine, serialUsed);
  if (netBtOn) tokenReadStream(netBt, btLine, btUsed);
}

static void waitForCredentials() {
  tokenProvision = false;
  netBtStart("AquaTech_Proto");
  Serial.println("[NET] Envia NombreRed,clave");
  while (!wifiHasCredentials()) {
    tokenPumpLines();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  netBtStop();
}

static void radioRelease() {
  wifiRadioOff();
  vTaskDelay(pdMS_TO_TICKS(120));
  netRadioOn = false;
}

static void syncOnce() {
  if (!wifiHasCredentials()) return;
  if (netBtOn) netBtStop();
  netOverlayReady = false;
  netHoldUi = true;
  unsigned long waitUi = millis();
  while (!netOverlayReady && (long)(millis() - waitUi) < 3000) {
    tokenPumpLines();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  netRadioOn = true;
  vTaskDelay(pdMS_TO_TICKS(80));
  netPhase = 2;
  wifiBegin();
  unsigned long start = millis();
  while (!wifiConnected() && (long)(millis() - start) < 20000) {
    tokenPumpLines();
    if (!wifiHasCredentials()) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  bool gotRules = false;
  if (wifiConnected()) {
    netPhase = 3;
    Serial.printf("[NET] WiFi %s\n", wifiIp().c_str());
    if (authLogin(tokenHost, tokenEmail, tokenPassword)) {
      tokenLogged = true;
      gotRules = configPull(tokenHost, tokenEmail, tokenPassword, "sync");
    }
    configTime(netTzSeconds, 0, "pool.ntp.org", "time.nist.gov");
    unsigned long clockWait = millis();
    while (time(nullptr) < 1700000000 && (long)(millis() - clockWait) < 2000) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  } else {
    Serial.println("[NET] sin WiFi");
  }
  radioRelease();
  netHoldUi = false;
  netNeedsRedraw = true;
  if (tokenLogged) {
    netPhase = 4;
    Serial.println(gotRules ? "[NET] reglas listas. WiFi apagado." : "[NET] token listo. WiFi apagado.");
  } else {
    netPhase = 0;
  }
}

static void tokenTask(void*) {
  wifiLoad();
  if (!wifiHasCredentials()) waitForCredentials();
  syncOnce();
  unsigned long next = millis() + netSyncMs;
  netNextAt = next;
  for (;;) {
    tokenPumpLines();
    if (tokenProvision || !wifiHasCredentials()) {
      waitForCredentials();
      syncOnce();
      next = millis() + netSyncMs;
      netNextAt = next;
      continue;
    }
    if ((long)(millis() - next) >= 0) {
      Serial.println("[NET] consulta");
      syncOnce();
      next = millis() + netSyncMs;
      netNextAt = next;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

static void tokenStart(const char* host, const char* email, const char* password, const char* deviceId, unsigned long syncMs) {
  tokenHost = host;
  tokenEmail = email;
  tokenPassword = password;
  if (syncMs < 15000) syncMs = 15000;
  netSyncMs = syncMs;
  identityLoad(deviceId);
  xTaskCreatePinnedToCore(tokenTask, "token", 24576, nullptr, 1, nullptr, 0);
}
