#pragma once

#include <ArduinoJson.h>

#include "api_client.h"
#include "auth_manager.h"
#include "device_identity.h"

// Baja las reglas solo cuando el sello que manda el servidor es distinto al guardado.

static char pendingRules[4500] = "";
static volatile char netDeviceName[32] = {0};

static void netCopyName(char* dest, size_t n) {
  if (!dest || n < 2) return;
  size_t i = 0;
  for (; i + 1 < n; i++) {
    char c = netDeviceName[i];
    dest[i] = c;
    if (!c) return;
  }
  dest[n - 1] = 0;
}
static int netTzSeconds = -7 * 3600;
static volatile bool pendingRulesFlag = false;
static unsigned long configSyncMs = 60000;
static unsigned long configNextPull = 0;
static SemaphoreHandle_t rulesLock = nullptr;

static void rulesEnsureLock() {
  if (!rulesLock) rulesLock = xSemaphoreCreateMutex();
}

static void netPostRules(const char* json) {
  if (!json) return;
  rulesEnsureLock();
  if (xSemaphoreTake(rulesLock, pdMS_TO_TICKS(50)) != pdTRUE) return;
  strncpy(pendingRules, json, sizeof(pendingRules) - 1);
  pendingRules[sizeof(pendingRules) - 1] = 0;
  pendingRulesFlag = true;
  xSemaphoreGive(rulesLock);
}

static bool netTakeRules(char* dest, size_t n) {
  if (!pendingRulesFlag || !dest || n < 2) return false;
  rulesEnsureLock();
  if (xSemaphoreTake(rulesLock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  strncpy(dest, pendingRules, n - 1);
  dest[n - 1] = 0;
  pendingRulesFlag = false;
  xSemaphoreGive(rulesLock);
  return dest[0] != 0;
}

static void rememberTimeZone(const char* tz) {
  if (!tz || (tz[0] != '-' && tz[0] != '+' && (tz[0] < '0' || tz[0] > '9'))) return;
  int sign = 1;
  const char* p = tz;
  if (*p == '-') {
    sign = -1;
    p++;
  } else if (*p == '+') {
    p++;
  }
  int hours = atoi(p);
  int mins = 0;
  const char* colon = strchr(p, ':');
  if (colon) mins = atoi(colon + 1);
  netTzSeconds = sign * (hours * 3600 + mins * 60);
}

static void rememberDeviceName(const char* name) {
  if (!name || !name[0]) return;
  char current[32];
  netCopyName(current, sizeof(current));
  if (strcmp(current, name) == 0) return;
  size_t i = 0;
  for (; name[i] && i + 1 < sizeof(netDeviceName); i++) netDeviceName[i] = name[i];
  netDeviceName[i] = 0;
  Serial.printf("[NET] nombre %s\n", name);
}

static bool keyIsExactName(const String& response, int quote) {
  if (quote < 0 || quote + 6 > (int)response.length()) return false;
  if (memcmp(response.c_str() + quote, "\"name\"", 6) != 0) return false;
  if (quote > 0) {
    char prev = response[quote - 1];
    if (prev == '_' || (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9')) return false;
  }
  return true;
}

// El nombre del controlador es hermano de rules, no un campo de un medidor.
// Puede venir antes o despues del bloque de reglas.
static void rememberControllerName(const String& response) {
  int depth = 0;
  bool inString = false;
  bool escape = false;
  int n = (int)response.length();
  int bestDepth = 99;
  char best[32] = "";
  for (int i = 0; i < n; i++) {
    char c = response[i];
    if (inString) {
      if (escape) {
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == '"') inString = false;
      continue;
    }
    if (c == '"') {
      if (depth >= 1 && depth <= 2 && keyIsExactName(response, i)) {
        int colon = response.indexOf(':', i + 6);
        int q1 = colon < 0 ? -1 : response.indexOf('"', colon);
        if (colon > i && colon - i < 12 && q1 > colon && q1 - colon < 8) {
          int q2 = q1 + 1;
          while (q2 < n) {
            if (response[q2] == '\\' && q2 + 1 < n) {
              q2 += 2;
              continue;
            }
            if (response[q2] == '"') break;
            q2++;
          }
          int len = q2 < n ? q2 - q1 - 1 : 0;
          if (len > 0 && depth < bestDepth) {
            if (len > 31) len = 31;
            for (int k = 0; k < len; k++) best[k] = response[q1 + 1 + k];
            best[len] = 0;
            bestDepth = depth;
          }
        }
      }
      inString = true;
      continue;
    }
    if (c == '{' || c == '[') depth++;
    else if ((c == '}' || c == ']') && depth > 0) depth--;
  }
  if (best[0]) rememberDeviceName(best);
}

static String rulesWithName(const String& rules) {
  char name[32];
  netCopyName(name, sizeof(name));
  if (!name[0]) return rules;
  int start = rules.indexOf('{');
  if (start < 0) return rules;
  String out;
  out.reserve(rules.length() + 48);
  out = rules.substring(0, start + 1);
  out += "\"controller_name\":\"";
  for (int i = 0; name[i]; i++) {
    char c = name[i];
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  out += "\"";
  if (start + 1 < (int)rules.length() && rules[start + 1] != '}') out += ',';
  out += rules.substring(start + 1);
  return out;
}

static bool configPull(const char* host, const char* email, const char* password, const char* revision) {
  if (!authEnsure(host, email, password)) return false;
  String path = String("/api/v1.0/controllers/") + deviceId;
  String response;
  int code = apiCall(host, "GET", path, "", savedToken, response);
  if (code == 401) {
    identityClearToken();
    if (!authLogin(host, email, password)) return false;
    code = apiCall(host, "GET", path, "", savedToken, response);
  }
  if (code < 200 || code >= 300) {
    Serial.printf("[NET] controlador HTTP %d\n", code);
    return false;
  }

  String rules;
  rememberControllerName(response);
  DynamicJsonDocument doc(16384);
  if (!deserializeJson(doc, response)) {
    rememberDeviceName(doc["name"] | "");
    rememberTimeZone(doc["time_zone"] | "");
    if (doc["rules"].is<JsonObject>()) serializeJson(doc["rules"], rules);
  } else {
    int at = response.indexOf("\"rules\"");
    int start = at < 0 ? -1 : response.indexOf('{', at);
    int depth = 0;
    for (int i = start; start >= 0 && i < (int)response.length(); i++) {
      if (response[i] == '{') depth++;
      else if (response[i] == '}') {
        depth--;
        if (depth == 0) {
          rules = response.substring(start, i + 1);
          break;
        }
      }
    }
  }
  if (rules.length() < 3) {
    Serial.println("[NET] la respuesta no trae reglas");
    return false;
  }
  char shown[32];
  netCopyName(shown, sizeof(shown));
  if (!shown[0]) {
    int at = response.indexOf("\"name\"");
    Serial.printf("[NET] la respuesta no trae nombre (%u bytes, nameAt %d)\n", (unsigned)response.length(), at);
  }
  String posted = rulesWithName(rules);
  if (posted.length() >= sizeof(pendingRules)) {
    Serial.printf("[NET] reglas demasiado grandes (%u)\n", (unsigned)posted.length());
    return false;
  }
  identitySaveRules(revision, posted.c_str());
  netPostRules(posted.c_str());
  netCopyName(shown, sizeof(shown));
  Serial.printf("[NET] reglas recibidas %u bytes, nombre %s\n", (unsigned)posted.length(), shown);
  return true;
}

static void configConsider(const char* host, const char* email, const char* password, const char* revision, unsigned long syncMs) {
  if (syncMs >= 15000) configSyncMs = syncMs;
  if (!revision || !revision[0]) return;
  if (strcmp(revision, savedRevision) == 0) return;
  if (configNextPull && (long)(millis() - configNextPull) < 0) return;
  if (configPull(host, email, password, revision)) configNextPull = millis() + configSyncMs;
  else configNextPull = millis() + 15000;
}
