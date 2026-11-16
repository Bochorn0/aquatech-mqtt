#pragma once

#include <ArduinoJson.h>
#include <time.h>

#include "api_client.h"
#include "device_identity.h"

// El token se guarda y se reutiliza. Solo se pide otro si falta, vencio o la API responde 401.

static long authReadExp(const char* token) {
  if (!token) return 0;
  const char* a = strchr(token, '.');
  if (!a) return 0;
  const char* b = strchr(a + 1, '.');
  if (!b) return 0;
  String payload;
  payload.reserve(b - a);
  for (const char* p = a + 1; p < b; p++) {
    if (*p == '-') payload += '+';
    else if (*p == '_') payload += '/';
    else payload += *p;
  }
  while (payload.length() % 4) payload += '=';
  String json;
  json.reserve(payload.length());
  int val = 0;
  int valb = -8;
  for (int i = 0; i < (int)payload.length(); i++) {
    char c = payload[i];
    if (c == '=') break;
    int d = -1;
    if (c >= 'A' && c <= 'Z') d = c - 'A';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
    else if (c >= '0' && c <= '9') d = c - '0' + 52;
    else if (c == '+') d = 62;
    else if (c == '/') d = 63;
    if (d < 0) continue;
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      json += (char)((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, json)) return 0;
  return doc["exp"] | 0;
}

static bool authTokenFresh() {
  if (!savedToken[0]) return false;
  time_t now = time(nullptr);
  if (now < 1700000000 || savedTokenExp <= 0) return true;
  return now + 60 < savedTokenExp;
}

static bool authLogin(const char* host, const char* email, const char* password) {
  if (!email || !password || !password[0] || strcmp(password, "CHANGE_ME") == 0) return false;
  StaticJsonDocument<192> doc;
  doc["email"] = email;
  doc["password"] = password;
  String payload;
  serializeJson(doc, payload);
  String response;
  int code = apiCall(host, "POST", "/api/v1.0/auth/login", payload, "", response);
  if (code < 200 || code >= 300) {
    identityClearToken();
    Serial.printf("[NET] login sin token (%d)\n", code);
    return false;
  }
  int key = response.indexOf("\"token\"");
  int colon = key < 0 ? -1 : response.indexOf(':', key);
  int q1 = colon < 0 ? -1 : response.indexOf('"', colon);
  int q2 = q1 < 0 ? -1 : response.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 <= q1) {
    identityClearToken();
    Serial.println("[NET] login sin token");
    return false;
  }
  String token = response.substring(q1 + 1, q2);
  identitySaveToken(token.c_str(), authReadExp(token.c_str()));
  Serial.println("[NET] token ok");
  return true;
}

static bool authEnsure(const char* host, const char* email, const char* password) {
  if (authTokenFresh()) return true;
  return authLogin(host, email, password);
}
