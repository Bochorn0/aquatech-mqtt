#pragma once

#include <Preferences.h>
#include <string.h>

// Identidad, token y ultima configuracion. Nada de esto se pide a la red para que el equipo arranque.

static char deviceId[48] = "";
static char savedRevision[24] = "";
static char savedToken[900] = "";
static long savedTokenExp = 0;

static void identityLoad(const char* fallbackId) {
  Preferences prefs;
  if (!prefs.begin("aquatech", true)) {
    strncpy(deviceId, fallbackId ? fallbackId : "", sizeof(deviceId) - 1);
    return;
  }
  String id = prefs.getString("device_id", fallbackId ? fallbackId : "");
  String revision = prefs.getString("revision", "");
  String token = prefs.getString("jwt", "");
  savedTokenExp = prefs.getLong("jwt_exp", 0);
  prefs.end();
  strncpy(deviceId, id.c_str(), sizeof(deviceId) - 1);
  strncpy(savedRevision, revision.c_str(), sizeof(savedRevision) - 1);
  strncpy(savedToken, token.c_str(), sizeof(savedToken) - 1);
  deviceId[sizeof(deviceId) - 1] = 0;
  savedRevision[sizeof(savedRevision) - 1] = 0;
  savedToken[sizeof(savedToken) - 1] = 0;
}

static void identitySaveId(const char* id) {
  strncpy(deviceId, id ? id : "", sizeof(deviceId) - 1);
  deviceId[sizeof(deviceId) - 1] = 0;
  Preferences prefs;
  if (!prefs.begin("aquatech", false)) return;
  prefs.putString("device_id", deviceId);
  prefs.end();
}

static bool identityReadRules(char* dest, size_t n) {
  if (!dest || n < 2) return false;
  dest[0] = 0;
  Preferences prefs;
  if (!prefs.begin("aquatech", true)) return false;
  String rules = prefs.getString("rules", "");
  prefs.end();
  if (!rules.length()) return false;
  strncpy(dest, rules.c_str(), n - 1);
  dest[n - 1] = 0;
  return true;
}

static void identitySaveRules(const char* revision, const char* rules) {
  strncpy(savedRevision, revision ? revision : "", sizeof(savedRevision) - 1);
  savedRevision[sizeof(savedRevision) - 1] = 0;
  Preferences prefs;
  if (!prefs.begin("aquatech", false)) return;
  prefs.putString("revision", savedRevision);
  if (rules && strlen(rules) < 3500) prefs.putString("rules", rules);
  prefs.end();
}

static void identitySaveToken(const char* token, long exp) {
  strncpy(savedToken, token ? token : "", sizeof(savedToken) - 1);
  savedToken[sizeof(savedToken) - 1] = 0;
  savedTokenExp = exp;
  Preferences prefs;
  if (!prefs.begin("aquatech", false)) return;
  prefs.putString("jwt", savedToken);
  prefs.putLong("jwt_exp", savedTokenExp);
  prefs.end();
}

static void identityClearToken() {
  savedToken[0] = 0;
  savedTokenExp = 0;
  Preferences prefs;
  if (!prefs.begin("aquatech", false)) return;
  prefs.remove("jwt");
  prefs.remove("jwt_exp");
  prefs.end();
}

static bool identityReadyForApi() {
  if (!deviceId[0] || strcmp(deviceId, "CAMBIA_DEVICE_ID") == 0) return false;
  return true;
}
