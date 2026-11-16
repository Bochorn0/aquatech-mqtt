#pragma once

#include <Preferences.h>
#include <WiFi.h>

// Credenciales de la red. El SDK tambien las recuerda; aqui queda la copia que decide si hay que abrir Bluetooth.

static char wifiSsid[33] = "";
static char wifiPass[65] = "";

static void wifiLoad() {
  Preferences prefs;
  if (!prefs.begin("aquatech", true)) return;
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  strncpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid) - 1);
  strncpy(wifiPass, pass.c_str(), sizeof(wifiPass) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = 0;
  wifiPass[sizeof(wifiPass) - 1] = 0;
}

static bool wifiHasCredentials() {
  return wifiSsid[0] != 0;
}

static void wifiSave(const char* ssid, const char* pass) {
  strncpy(wifiSsid, ssid ? ssid : "", sizeof(wifiSsid) - 1);
  strncpy(wifiPass, pass ? pass : "", sizeof(wifiPass) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = 0;
  wifiPass[sizeof(wifiPass) - 1] = 0;
  Preferences prefs;
  if (!prefs.begin("aquatech", false)) return;
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.end();
}

static void wifiClear() {
  wifiSsid[0] = 0;
  wifiPass[0] = 0;
  Preferences prefs;
  if (prefs.begin("aquatech", false)) {
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
  }
  WiFi.disconnect(true, true);
}

static void wifiRadioOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

static void wifiBegin() {
  if (!wifiHasCredentials()) return;
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid, wifiPass);
}

static bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

static String wifiIp() {
  if (!wifiConnected()) return "";
  return WiFi.localIP().toString();
}
