#pragma once

#include <WiFiClientSecure.h>

// Una solicitud a la vez, y se cierra al terminar. No queda un servidor escuchando en el equipo.

static int apiHttpCode(const String& raw) {
  int s1 = raw.indexOf(' ');
  int s2 = raw.indexOf(' ', s1 + 1);
  if (s1 < 0 || s2 <= s1) return -1;
  return raw.substring(s1 + 1, s2).toInt();
}

static String apiHttpBody(const String& raw) {
  int bodyPos = raw.indexOf("\r\n\r\n");
  if (bodyPos < 0) return "";
  String headers = raw.substring(0, bodyPos);
  String body = raw.substring(bodyPos + 4);
  String headersLower = headers;
  headersLower.toLowerCase();
  if (headersLower.indexOf("transfer-encoding: chunked") < 0) return body;

  String out;
  int i = 0;
  while (i < (int)body.length()) {
    int lineEnd = body.indexOf("\r\n", i);
    if (lineEnd < 0) break;
    int size = (int)strtol(body.c_str() + i, nullptr, 16);
    if (size <= 0) break;
    int dataStart = lineEnd + 2;
    if (dataStart + size > (int)body.length()) break;
    out += body.substring(dataStart, dataStart + size);
    i = dataStart + size + 2;
  }
  if (out.length() > 0 && (out[0] == '{' || out[0] == '[')) return out;
  int brace = body.indexOf('{');
  if (brace >= 0) return body.substring(brace);
  return out;
}

static int apiCall(const char* host, const char* method, const String& path, const String& body, const char* bearer, String& responseBody) {
  responseBody = "";
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(12);
  client.setTimeout(8000);
  if (!client.connect(host, 443)) {
    Serial.printf("[NET] TLS fallo %s\n", path.c_str());
    return -1;
  }

  String req;
  req.reserve(body.length() + 256);
  req += method;
  req += " ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += host;
  req += "\r\nContent-Type: application/json\r\nConnection: close\r\n";
  if (bearer && bearer[0]) {
    req += "Authorization: Bearer ";
    req += bearer;
    req += "\r\n";
  }
  req += "Content-Length: ";
  req += String(body.length());
  req += "\r\n\r\n";
  req += body;
  client.print(req);

  unsigned long start = millis();
  String raw;
  raw.reserve(2048);
  while (millis() - start < 8000) {
    while (client.available()) {
      raw += (char)client.read();
      if (raw.length() > 24000) break;
    }
    if (!client.connected() && !client.available()) break;
    delay(5);
  }
  client.stop();
  int code = apiHttpCode(raw);
  responseBody = apiHttpBody(raw);
  Serial.printf("[NET] %s %s -> %d\n", method, path.c_str(), code);
  return code;
}
