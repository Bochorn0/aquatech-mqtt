#pragma once

#include <BluetoothSerial.h>
#include <ArduinoJson.h>
#include <time.h>

#include "wifi_manager.h"
#include "device_identity.h"
#include "auth_manager.h"
#include "config_sync.h"

// 0 sin red, 1 Bluetooth de alta, 2 intentando WiFi, 3 WiFi sin API, 4 API.
static volatile int netPhase = 0;
static BluetoothSerial netBt;
static bool netBtOn = false;
static bool netProvisionAsked = false;
static unsigned long netProvisionUntil = 0;

static const char* netHost = "";
static const char* netEmail = "";
static const char* netPassword = "";

static void netBtStart(const char* name) {
  if (netBtOn) return;
  if (!netBt.begin(name)) {
    Serial.println("[NET] Bluetooth no abrio");
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

static void netHandleLine(String line) {
  line.trim();
  if (!line.length()) return;
  if (line == "CLEAR_WIFI") {
    wifiClear();
    netProvisionAsked = true;
    Serial.println("[NET] WiFi borrado");
    return;
  }
  if (line == "PROVISION") {
    netProvisionAsked = true;
    Serial.println("[NET] alta abierta 3 min");
    return;
  }
  if (line.startsWith("ID ")) {
    String id = line.substring(3);
    id.trim();
    if (id.length() > 0 && id.length() < (int)sizeof(deviceId)) {
      identitySaveId(id.c_str());
      savedRevision[0] = 0;
      Serial.printf("[NET] ID %s\n", deviceId);
    }
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
  netProvisionUntil = 0;
  netBtStop();
  wifiBegin();
  netPhase = 2;
  Serial.printf("[NET] red %s\n", wifiSsid);
}

static void netReadStream(Stream& stream) {
  static char line[96];
  static int used = 0;
  while (stream.available()) {
    char c = (char)stream.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[used] = 0;
      used = 0;
      netHandleLine(String(line));
      continue;
    }
    if (used < (int)sizeof(line) - 1) line[used++] = c;
  }
}

static bool netHeartbeat() {
  if (!identityReadyForApi()) return false;
  if (!authEnsure(netHost, netEmail, netPassword)) return false;
  String path = String("/api/v1.0/controllers/") + deviceId + "?brief=1";
  String payload = "{\"ip\":\"" + wifiIp() + "\"";
  time_t nowSec = time(nullptr);
  if (nowSec > 1700000000) {
    char epochBuf[24];
    snprintf(epochBuf, sizeof(epochBuf), "%lld", (long long)nowSec * 1000LL);
    payload += ",\"last_time_active\":";
    payload += epochBuf;
  }
  payload += "}";
  String response;
  int code = apiCall(netHost, "PATCH", path, payload, savedToken, response);
  if (code == 401) {
    identityClearToken();
    if (!authLogin(netHost, netEmail, netPassword)) return false;
    code = apiCall(netHost, "PATCH", path, payload, savedToken, response);
  }
  if (code < 200 || code >= 300) return false;
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, response)) return true;
  const char* revision = doc["revision"] | "";
  unsigned long syncMs = doc["sync_ms"] | configSyncMs;
  configConsider(netHost, netEmail, netPassword, revision, syncMs);
  return true;
}

static bool netCanLogin() {
  return identityReadyForApi() && netPassword && netPassword[0] && strcmp(netPassword, "CHANGE_ME") != 0;
}

static void netTask(void*) {
  wifiLoad();
  unsigned long nextWifi = 0;
  unsigned long nextHeart = 0;
  int wifiFails = 0;
  bool clockAsked = false;
  bool loginWarned = false;
  if (!wifiHasCredentials()) netBtStart("AquaTech_Proto");
  else {
    netPhase = 2;
    wifiBegin();
    nextWifi = millis() + 12000;
  }

  for (;;) {
    unsigned long now = millis();
    netReadStream(Serial);
    if (netBtOn) netReadStream(netBt);
    if (netProvisionAsked) {
      netProvisionAsked = false;
      netProvisionUntil = now + 180000;
      netBtStart("AquaTech_Proto");
    }
    if (netProvisionUntil && (long)(now - netProvisionUntil) >= 0 && wifiHasCredentials()) {
      netProvisionUntil = 0;
      netBtStop();
    }

    if (!wifiConnected()) {
      if (netPhase > 2) netPhase = wifiHasCredentials() ? 2 : 1;
      if (wifiHasCredentials() && (long)(now - nextWifi) >= 0) {
        wifiBegin();
        unsigned long wait = 5000ul << (wifiFails > 4 ? 4 : wifiFails);
        if (wait > 60000) wait = 60000;
        nextWifi = now + wait;
        if (wifiFails < 6) wifiFails++;
      }
      clockAsked = false;
    } else {
      if (netBtOn && !netProvisionUntil) netBtStop();
      if (!clockAsked) {
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        clockAsked = true;
        Serial.printf("[NET] WiFi %s\n", wifiIp().c_str());
        Serial.println("[NET] Con WiFi, GPIO 12 y 13 pueden leer mal.");
      }
      wifiFails = 0;
      if (netPhase < 3) netPhase = 3;
      if (!netCanLogin()) {
        if (!loginWarned) {
          loginWarned = true;
          Serial.println("[NET] falta el ID o la clave. Sigue con la configuracion local.");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      if ((long)(now - nextHeart) >= 0) {
        if (netHeartbeat()) {
          netPhase = 4;
          nextHeart = millis() + 2500;
        } else {
          if (netPhase == 4) netPhase = 3;
          nextHeart = millis() + 10000;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

static void connectivityStart(const char* host, const char* email, const char* password, const char* fallbackId) {
  netHost = host;
  netEmail = email;
  netPassword = password;
  identityLoad(fallbackId);
  if (!deviceId[0]) identitySaveId(fallbackId ? fallbackId : "");
  rulesEnsureLock();
  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, nullptr, 0);
}

static int connectivityPhase() {
  return netPhase;
}
