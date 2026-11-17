// ================== AQUATECH — PROTOTIPO FULL V001 ==================
// Al encender usa esta distribucion. Si hay WiFi, pide las reglas y las
// vuelve a pedir cada NET_SYNC_MS (5 minutos). Sin clave abre Bluetooth
// AquaTech_Proto y sigue con estos valores.
// El WiFi solo consulta y se apaga: los sensores y relevadores trabajan sin radio.
// SCREEN_CHECK_MS revisa la pantalla. Solo se redibuja si esta negra o pixelada.
// Durante la consulta la pantalla muestra el aviso y no se redibuja.
// En el IDE: Partition Scheme = Huge APP (3MB) si el esquema de 1.2MB no alcanza.
//
// Distribucion default. ON = HIGH si cfg.relayActiveHigh es true.
//   1 GPIO 32  bomba cruda       interruptor GPIO 16 a GND
//   2 GPIO 33  valvula           el mismo interruptor GPIO 16
//   3 GPIO 25  flush             30 s cada 12 h
//   4 GPIO 26  cloro             flujo del medidor 2 (GPIO 19) mayor a 0
//   5 GPIO 27  sin uso           apagado
//   6 GPIO 14  bomba purificada  interruptor GPIO 22 a GND
// Presion, cada uno con resistencia de 150 ohm. No encienden relevadores:
//   S1 GPIO 34  0.812 MPa    S2 GPIO 35  1.983 MPa
//   S3 GPIO 13  0.915 MPa    S4 GPIO 12  2.327 MPa
//   S5 sin pin. Se muestra en la pantalla.
// GPIO 12 y 13 tambien son relevadores. Se leen solo con el WiFi apagado
// y vuelven a LOW enseguida.
//
// Flujos fisicos, pulso a GND, 0.1 L por pulso:
//   1 Agua cruda, GPIO 4. Si no hay flujo, no arranca equipo.
//   2 Cruda a tienda, GPIO 19. Mayor a 0 enciende el cloro.
// Rechazo, produccion y purificada a tienda se leen en la plataforma.
// El portal numera distinto: 1 cruda, 2 flush, 3 cloro, 4 purificada.
// Esas reglas todavia no se copian a estos GPIO. El detalle esta en CONTROLADOR.md.
//
// Pantalla: rotacion 1, MADCTL 0xE8.
// No usar: GPIO 2, 5, 18, 21 y 23 (pantalla), GPIO 15 (arranque).

#include <TFT_eSPI.h>
#include <esp_bt.h>
#include "net/wifi_token.h"

static const char* NET_API_HOST = "lccapp-aefcbwh0ecd7b8cz.canadacentral-01.azurewebsites.net";
static const char* NET_LOGIN_EMAIL = "esp32@lcc.com.mx";
static const char* NET_LOGIN_PASSWORD = "CHANGE_ME"
static const char* NET_DEVICE_ID = "CTRL-1794540273392-6bdpn134h";
static const unsigned long NET_SYNC_MS = 300000;
static const unsigned long SCREEN_CHECK_MS = 60000;

enum RuleMode : uint8_t {
  MODE_OFF = 0,
  MODE_SWITCH,
  MODE_AND,
  MODE_TIMER,
  MODE_FLOW,
  MODE_DIFF,
  MODE_PRESSURE
};

enum FlowOrigin : uint8_t {
  FLOW_PIN = 0,
  FLOW_API
};

struct RuleConfig {
  bool enabled;
  RuleMode mode;
  int inputA;
  int inputB;
  int flowPin;
  float minLpm;
  float flowHystLpm;
  float litersPerPulse;
  int sensorA;
  int sensorB;
  float diffPsi;
  float hystPsi;
  unsigned long intervalMs;
  unsigned long durationMs;
  int meterIndex;
  bool portalOn;
  float minPsi;
  float maxPsi;
  uint8_t sensorMask;
};

struct FlowMeter {
  FlowOrigin origin;
  int pin;
  float litersPerPulse;
  char name[16];
};

struct Config {
  bool relayActiveHigh;
  bool inputActiveLow;
  int relayPin[6];
  int heldOffPin;
  float shuntOhms;
  float psiGain;
  int sensorPin[4];
  float sensorSpanMpa[4];
  unsigned long debounceMs;
  unsigned long readMs;
  unsigned long reportMs;
  unsigned long drawMs;
  unsigned long relayDwellMs;
  unsigned long flowWindowMs;
  RuleConfig rule[6];
  FlowMeter meter[2];
};

// Orden fisico: cruda, valvula, flush, cloro, libre, purificada.
Config cfg = {
  true,
  true,
  {32, 33, 25, 26, 27, 14},
  12,
  150.0f,
  18.0f / 22.0f,
  {34, 35, 13, 12},
  {0.812f, 1.983f, 0.915f, 2.327f},
  40,
  400,
  1000,
  400,
  800,
  5000,
  {
    {true,  MODE_SWITCH, 16, -1, -1, 0, 0, 0,    -1, -1, 0,  0,    0,        0,     -1, false, 0,  0,  0},
    {true,  MODE_SWITCH, 16, -1, -1, 0, 0, 0,    -1, -1, 0,  0,    0,        0,     -1, false, 0,  0,  0},
    {true,  MODE_TIMER,  -1, -1, -1, 0, 0, 0,    -1, -1, 0,  0,    43200000, 30000, -1, false, 0,  0,  0},
    {true,  MODE_FLOW,   -1, -1, -1, 0, 0, 0,    -1, -1, 0,  0,    0,        0,      1, false, 0,  0,  0},
    {false, MODE_OFF,    -1, -1, -1, 0, 0, 0,    -1, -1, 0,  0,    0,        0,     -1, false, 0,  0,  0},
    {true,  MODE_SWITCH, 22, -1, -1, 0, 0, 0,    -1, -1, 0,  0,    0,        0,     -1, false, 0,  0,  0}
  },
  {
    {FLOW_PIN, 4, 0.1f, "Agua cruda"},
    {FLOW_PIN, 19, 0.1f, "Cruda a tienda"}
  }
};

static const int RELAY_COUNT = 6;
static const int SENSOR_COUNT = 4;
static const int PRESSURE_CARDS = 5;
static const char* RELAY_NAME[6] = {"Cruda", "Valvula", "Flush", "Cloro", "Presion", "Pura"};

TFT_eSPI tft = TFT_eSPI();

struct Layout {
  int headerH;
  int pressureTitleY;
  int pressureY;
  int pressureH;
  int flowTitleY;
  int flowY;
  int flowH;
  int relayTitleY;
  int relayTop;
  int relayH;
};

Layout ui;

struct Debounce {
  int pin;
  bool raw;
  bool stable;
  unsigned long since;
};

Debounce inputState[8];
int inputCount = 0;

float psiLive[4] = {0, 0, 0, 0};
float milliampLive[4] = {0, 0, 0, 0};
bool pressureValid[4] = {false, false, false, false};
float flowLpm[2] = {0, 0};
bool relayOn[6] = {false, false, false, false, false, false};
unsigned long relayChangedAt[6] = {0, 0, 0, 0, 0, 0};
bool flushRunning = false;
unsigned long flushStartedAt = 0;
unsigned long nextFlushAt = 0;
unsigned long flowWindowStart = 0;
unsigned long lastRead = 0;
unsigned long lastReport = 0;
unsigned long nextScreenCheck = 0;
bool screenReadable = false;
bool screenProbeWarned = false;
bool displayReady = false;
float shownPsi[4] = {-999, -999, -999, -999};
bool shownValid[4] = {false, false, false, false};
float shownLpm[2] = {-999, -999};
bool shownRelay[6] = {false, false, false, false, false, false};
char shownHint[6][12] = {{0}, {0}, {0}, {0}, {0}, {0}};
bool shownPump = false;
int shownNet = -2;
char shownSync[32] = "";
char shownName[32] = "";
int boundFlowPin[2] = {-1, -1};

volatile uint32_t flowPulses[2] = {0, 0};
volatile uint32_t flowLastUs[2] = {0, 0};

uint16_t pageColor() { return tft.color565(232, 239, 244); }
uint16_t inkColor() { return tft.color565(24, 44, 60); }
uint16_t muteColor() { return tft.color565(112, 130, 144); }
uint16_t cardLine() { return tft.color565(186, 202, 214); }
uint16_t navyColor() { return tft.color565(12, 52, 84); }
uint16_t onColor() { return tft.color565(16, 132, 78); }
uint16_t onCard() { return tft.color565(226, 244, 233); }

void logger(const char* title, const char* format, ...) {
  char buffer[180];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Serial.printf("[%s] %s\n", title, buffer);
}

bool pinUsed(int pin, int* taken, int count) {
  for (int i = 0; i < count; i++) {
    if (taken[i] == pin) return true;
  }
  return false;
}

void track(int* taken, int& count, int pin) {
  if (pin < 0 || pinUsed(pin, taken, count)) return;
  taken[count++] = pin;
}

bool configPinsConflict() {
  int taken[24];
  int count = 0;
  bool clash = false;
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (pinUsed(cfg.relayPin[i], taken, count)) clash = true;
    track(taken, count, cfg.relayPin[i]);
  }
  if (pinUsed(cfg.heldOffPin, taken, count)) clash = true;
  track(taken, count, cfg.heldOffPin);
  for (int i = 0; i < SENSOR_COUNT; i++) {
    int pin = cfg.sensorPin[i];
    bool shared = pin == 12 || pin == 13;
    if (pinUsed(pin, taken, count) && !shared) clash = true;
    track(taken, count, pin);
  }
  int inputs[5] = {
    cfg.rule[0].inputA, cfg.rule[1].inputB, cfg.rule[4].inputA, 2, 5
  };
  int extraTft[3] = {18, 21, 23};
  for (int i = 0; i < 2; i++) {
    if (cfg.meter[i].origin != FLOW_PIN || cfg.meter[i].pin < 0) continue;
    if (pinUsed(cfg.meter[i].pin, taken, count)) clash = true;
    track(taken, count, cfg.meter[i].pin);
  }
  for (int i = 0; i < 5; i++) {
    if (inputs[i] >= 0 && pinUsed(inputs[i], taken, count)) clash = true;
    track(taken, count, inputs[i]);
  }
  for (int i = 0; i < 3; i++) {
    if (pinUsed(extraTft[i], taken, count)) clash = true;
  }
  if (cfg.rule[2].durationMs >= cfg.rule[2].intervalMs) clash = true;
  return clash;
}

void writeRelay(int pin, bool on) {
  bool high = cfg.relayActiveHigh ? on : !on;
  digitalWrite(pin, high ? HIGH : LOW);
}

void parkRelayPin(int pin) {
  if (pin < 0) return;
  pinMode(pin, OUTPUT);
  writeRelay(pin, false);
}

void initOutputs() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    parkRelayPin(cfg.relayPin[i]);
    relayOn[i] = false;
  }
  parkRelayPin(12);
  parkRelayPin(13);
}

int findInput(int pin) {
  for (int i = 0; i < inputCount; i++) {
    if (inputState[i].pin == pin) return i;
  }
  return -1;
}

void addInput(int pin) {
  if (pin < 0 || findInput(pin) >= 0 || inputCount >= 8) return;
  pinMode(pin, INPUT_PULLUP);
  inputState[inputCount].pin = pin;
  inputState[inputCount].raw = digitalRead(pin) == LOW;
  inputState[inputCount].stable = inputState[inputCount].raw;
  inputState[inputCount].since = millis();
  inputCount++;
}

void IRAM_ATTR onFlowPulse0() {
  uint32_t now = micros();
  if ((uint32_t)(now - flowLastUs[0]) < 2000) return;
  flowLastUs[0] = now;
  flowPulses[0]++;
}

void IRAM_ATTR onFlowPulse1() {
  uint32_t now = micros();
  if ((uint32_t)(now - flowLastUs[1]) < 2000) return;
  flowLastUs[1] = now;
  flowPulses[1]++;
}

void bindFlowMeters() {
  for (int i = 0; i < 2; i++) {
    if (boundFlowPin[i] < 0) continue;
    detachInterrupt(digitalPinToInterrupt(boundFlowPin[i]));
    boundFlowPin[i] = -1;
  }
  for (int i = 0; i < 2; i++) {
    int pin = cfg.meter[i].pin;
    if (cfg.meter[i].origin != FLOW_PIN || pin < 0) continue;
    if (i == 1 && cfg.meter[0].origin == FLOW_PIN && cfg.meter[0].pin == pin) continue;
    pinMode(pin, INPUT_PULLUP);
    if (i == 0) attachInterrupt(digitalPinToInterrupt(pin), onFlowPulse0, FALLING);
    else attachInterrupt(digitalPinToInterrupt(pin), onFlowPulse1, FALLING);
    boundFlowPin[i] = pin;
  }
}

void initInputs() {
  addInput(cfg.rule[0].inputA);
  addInput(cfg.rule[1].inputA);
  addInput(cfg.rule[1].inputB);
  addInput(cfg.rule[4].inputA);
  bindFlowMeters();
}

void initDisplay() {
  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.writecommand(0x2A);
  tft.writedata(0x00); tft.writedata(0x00);
  tft.writedata(0x01); tft.writedata(0xDF);
  tft.writecommand(0x2B);
  tft.writedata(0x00); tft.writedata(0x00);
  tft.writedata(0x01); tft.writedata(0x3F);
  tft.writecommand(0x36);
  tft.writedata(0xE8);
  displayReady = true;
}

void computeLayout() {
  ui.headerH = 34;
  int y = ui.headerH + 6;
  ui.pressureTitleY = y;
  y += 16;
  ui.pressureY = y;
  ui.pressureH = 74;
  y += ui.pressureH + 4;
  ui.flowTitleY = y;
  y += 16;
  ui.flowY = y;
  ui.flowH = 54;
  y += ui.flowH + 4;
  ui.relayTitleY = y;
  y += 16;
  ui.relayTop = y;
  ui.relayH = tft.height() - y - 4;
  if (ui.relayH < 64) ui.relayH = 64;
}

void paintCard(int x, int y, int w, int h, uint16_t fill, uint16_t border) {
  tft.fillRect(x, y, w, h, pageColor());
  tft.fillRoundRect(x, y, w, h, 6, fill);
  tft.drawRoundRect(x, y, w, h, 6, border);
}

void drawPumpIcon(int cx, int cy, bool on) {
  uint16_t body = on ? onColor() : muteColor();
  uint16_t blade = on ? TFT_WHITE : tft.color565(232, 239, 244);
  tft.fillCircle(cx - 3, cy, 9, body);
  tft.drawLine(cx - 3, cy, cx - 3, cy - 6, blade);
  tft.drawLine(cx - 3, cy, cx + 4, cy + 3, blade);
  tft.drawLine(cx - 3, cy, cx - 8, cy + 3, blade);
  tft.fillRect(cx + 6, cy - 3, 8, 5, body);
}

void drawValveIcon(int cx, int cy, bool on) {
  uint16_t body = on ? onColor() : muteColor();
  uint16_t pipe = on ? tft.color565(28, 150, 186) : tft.color565(176, 188, 196);
  tft.fillRect(cx - 13, cy - 2, 26, 4, pipe);
  tft.fillRoundRect(cx - 6, cy - 8, 12, 16, 2, body);
  tft.fillRoundRect(cx - 4, cy - 14, 8, 7, 2, on ? tft.color565(214, 156, 42) : muteColor());
  if (!on) tft.drawLine(cx - 3, cy - 3, cx + 3, cy + 3, TFT_WHITE);
}

void drawFlushIcon(int cx, int cy, bool on) {
  uint16_t c = on ? tft.color565(0, 138, 176) : muteColor();
  tft.drawCircle(cx, cy, 9, c);
  tft.drawCircle(cx, cy, 8, c);
  tft.fillTriangle(cx + 4, cy - 10, cx + 12, cy - 5, cx + 3, cy - 3, c);
  tft.fillCircle(cx - 1, cy + 2, 3, c);
}

void drawChlorineIcon(int cx, int cy, bool on) {
  uint16_t c = on ? tft.color565(72, 158, 58) : muteColor();
  tft.fillTriangle(cx, cy - 12, cx - 7, cy - 1, cx + 7, cy - 1, c);
  tft.fillCircle(cx, cy + 3, 7, c);
  if (on) tft.fillCircle(cx - 2, cy + 1, 2, TFT_WHITE);
}

void drawGaugeIcon(int cx, int cy, bool on) {
  uint16_t c = on ? onColor() : muteColor();
  tft.drawCircle(cx, cy, 10, c);
  tft.drawCircle(cx, cy, 9, c);
  tft.fillCircle(cx, cy, 2, c);
  if (on) tft.drawLine(cx, cy, cx + 6, cy - 6, c);
  else tft.drawLine(cx, cy, cx - 5, cy + 5, c);
}

void drawPureIcon(int cx, int cy, bool on) {
  uint16_t c = on ? tft.color565(22, 118, 186) : muteColor();
  tft.drawLine(cx - 9, cy - 8, cx + 9, cy - 8, c);
  tft.drawLine(cx - 9, cy - 8, cx - 3, cy + 1, c);
  tft.drawLine(cx + 9, cy - 8, cx + 3, cy + 1, c);
  tft.drawLine(cx - 3, cy + 1, cx + 3, cy + 1, c);
  tft.fillCircle(cx, cy + 8, on ? 4 : 3, c);
}

void drawEquipIcon(int index, int cx, int cy, bool on) {
  if (index == 0) drawPumpIcon(cx, cy, on);
  else if (index == 1) drawValveIcon(cx, cy, on);
  else if (index == 2) drawFlushIcon(cx, cy, on);
  else if (index == 3) drawChlorineIcon(cx, cy, on);
  else if (index == 4) drawGaugeIcon(cx, cy, on);
  else drawPureIcon(cx, cy, on);
}

void flowSourceLabel(const FlowMeter& meter, char* buf, size_t n) {
  if (meter.origin == FLOW_API) {
    char tag[6];
    int used = 0;
    for (int i = 0; meter.name[i] != 0 && used < 5; i++) tag[used++] = meter.name[i];
    tag[used] = 0;
    if (used == 0) snprintf(buf, n, "API");
    else snprintf(buf, n, "API-%s", tag);
    return;
  }
  if (meter.pin >= 0) snprintf(buf, n, "(%d)", meter.pin);
  else snprintf(buf, n, "s/pin");
}

void formatRemain(char* buf, size_t n, unsigned long ms) {
  unsigned long sec = (ms + 999) / 1000;
  if (sec >= 3600) snprintf(buf, n, "%luh%02lu", sec / 3600, (sec % 3600) / 60);
  else if (sec >= 60) snprintf(buf, n, "%lu:%02lu", sec / 60, sec % 60);
  else snprintf(buf, n, "%lus", sec);
}

void relayHint(int index, char* buf, size_t n) {
  RuleConfig& rule = cfg.rule[index];
  buf[0] = 0;
  if (rule.mode == MODE_SWITCH && rule.inputA >= 0) {
    snprintf(buf, n, "(%d)", rule.inputA);
  } else if (rule.mode == MODE_AND && rule.inputA >= 0 && rule.inputB >= 0) {
    snprintf(buf, n, "(%d+%d)", rule.inputA, rule.inputB);
  } else if (rule.mode == MODE_FLOW && rule.meterIndex >= 0 && rule.meterIndex < 2) {
    flowSourceLabel(cfg.meter[rule.meterIndex], buf, n);
  } else if (rule.mode == MODE_TIMER) {
    unsigned long now = millis();
    unsigned long target = flushRunning ? flushStartedAt + rule.intervalMs : nextFlushAt;
    long left = (long)(target - now);
    if (nextFlushAt == 0 || left < 0) left = 0;
    formatRemain(buf, n, (unsigned long)left);
  } else if (rule.mode == MODE_DIFF) {
    int a = rule.sensorA;
    int b = rule.sensorB;
    if (a < 0 || b < 0 || a >= SENSOR_COUNT || b >= SENSOR_COUNT || !pressureValid[a] || !pressureValid[b]) {
      snprintf(buf, n, "n/d");
    } else {
      snprintf(buf, n, "%.1f", fabs(psiLive[a] - psiLive[b]));
    }
  } else if (rule.mode == MODE_PRESSURE) {
    if (!rule.sensorMask) snprintf(buf, n, "s/sen");
    else snprintf(buf, n, "%.0f-%.0f", rule.minPsi, rule.maxPsi);
  }
}

void drawEquipCell(int x, int y, int w, int h, int index, bool on) {
  uint16_t fill = on ? onCard() : TFT_WHITE;
  uint16_t border = on ? onColor() : cardLine();
  paintCard(x, y, w, h, fill, border);
  if (on) tft.fillRect(x + 8, y + 1, w - 16, 3, onColor());
  drawEquipIcon(index, x + w / 2, y + 22, on);

  char hint[12];
  relayHint(index, hint, sizeof(hint));
  tft.setTextSize(1);
  tft.setTextColor(navyColor(), fill);
  int hintW = (int)strlen(hint) * 6;
  tft.setCursor(x + (w - hintW) / 2, y + 40);
  tft.print(hint);

  tft.setTextColor(inkColor(), fill);
  int nameW = (int)strlen(RELAY_NAME[index]) * 6;
  tft.setCursor(x + (w - nameW) / 2, y + h - 28);
  tft.print(RELAY_NAME[index]);

  const char* state = on ? "ON" : "OFF";
  tft.setTextColor(on ? onColor() : muteColor(), fill);
  tft.setCursor(x + (w - (int)strlen(state) * 6) / 2, y + h - 14);
  tft.print(state);

  shownRelay[index] = on;
  strncpy(shownHint[index], hint, sizeof(shownHint[index]) - 1);
  shownHint[index][sizeof(shownHint[index]) - 1] = 0;
}

void relayCellBox(int index, int& x, int& w) {
  const int gap = 5;
  w = (tft.width() - gap * (RELAY_COUNT + 1)) / RELAY_COUNT;
  x = gap + index * (w + gap);
}

void drawRelayRow() {
  if (ui.relayH < 52) return;
  for (int i = 0; i < RELAY_COUNT; i++) {
    int x = 0;
    int w = 0;
    relayCellBox(i, x, w);
    drawEquipCell(x, ui.relayTop, w, ui.relayH, i, relayOn[i]);
  }
}

void drawHeader() {
  uint16_t navy = navyColor();
  tft.fillRect(0, 0, tft.width(), ui.headerH, navy);
  char titleBuf[32];
  netCopyName(titleBuf, sizeof(titleBuf));
  const char* title = titleBuf[0] ? titleBuf : "...";
  int room = tft.width() - 132;
  int size = (int)strlen(title) * 12 <= room ? 2 : 1;
  int charW = size == 2 ? 12 : 6;
  int maxChars = room / charW;
  if (maxChars < 1) maxChars = 1;
  tft.setTextSize(size);
  tft.setTextColor(TFT_WHITE, navy);
  tft.setCursor(10, size == 2 ? 8 : 10);
  for (int i = 0; title[i] && i < maxChars; i++) tft.print(title[i]);
  strncpy(shownName, titleBuf, sizeof(shownName) - 1);
  shownName[sizeof(shownName) - 1] = 0;
  shownSync[0] = 0;
  drawSyncLine();
}

void clockText(char* buf, size_t n) {
  time_t clock = netUpdatedClock;
  if (netUpdatedAt && clock < 1700000000) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      clock = now - (time_t)((millis() - netUpdatedAt) / 1000);
      netUpdatedClock = clock;
    }
  }
  if (clock > 1700000000) {
    struct tm parts;
    localtime_r(&clock, &parts);
    snprintf(buf, n, "%02d:%02d", parts.tm_hour, parts.tm_min);
    return;
  }
  snprintf(buf, n, "--:--");
}

void drawSyncLine() {
  char ultima[8];
  char proxima[12];
  char line[28];
  int phase = netPhase;
  clockText(ultima, sizeof(ultima));
  if (netNextAt == 0 || (long)(netNextAt - millis()) <= 0) snprintf(proxima, sizeof(proxima), "0s");
  else formatRemain(proxima, sizeof(proxima), netNextAt - millis());
  if (phase == 1) snprintf(line, sizeof(line), "BT");
  else if (phase == 2) snprintf(line, sizeof(line), "...");
  else if (phase == 3) snprintf(line, sizeof(line), "WiFi");
  else snprintf(line, sizeof(line), "%s|%s", ultima, proxima);
  if (strcmp(shownSync, line) == 0) return;
  strncpy(shownSync, line, sizeof(shownSync) - 1);
  shownSync[sizeof(shownSync) - 1] = 0;
  shownNet = phase;
  uint16_t navy = navyColor();
  int x = tft.width() - 118;
  tft.fillRect(x - 4, 0, tft.width() - x + 4, ui.headerH, navy);
  tft.setTextSize(1);
  tft.setTextColor(tft.color565(168, 206, 230), navy);
  if (phase >= 1 && phase <= 3) {
    tft.setCursor(x, 12);
    tft.print(line);
    return;
  }
  tft.setCursor(x, 4);
  tft.print("Ultima: ");
  tft.print(ultima);
  tft.setCursor(x, 18);
  tft.print("Proxima: ");
  tft.print(proxima);
}

void floodPanel(uint16_t color) {
  tft.writecommand(0x2A);
  tft.writedata(0x00); tft.writedata(0x00);
  tft.writedata(0x01); tft.writedata(0xDF);
  tft.writecommand(0x2B);
  tft.writedata(0x00); tft.writedata(0x00);
  tft.writedata(0x01); tft.writedata(0x3F);
  tft.writecommand(0x2C);
  uint16_t line[480];
  for (int i = 0; i < 480; i++) line[i] = color;
  tft.startWrite();
  for (int y = 0; y < 320; y++) tft.pushColors(line, 480);
  tft.endWrite();
  tft.writecommand(0x36);
  tft.writedata(0xE8);
}

void drawWaitScreen() {
  uint16_t navy = navyColor();
  uint16_t veil = tft.color565(18, 36, 52);
  int w = tft.width();
  int h = tft.height();
  for (int y = 0; y < h; y += 2) tft.drawFastHLine(0, y, w, veil);
  const char* line1 = "Actualizando datos";
  const char* line2 = "por favor espere";
  int cardW = 210;
  int cardH = 52;
  int x = (w - cardW) / 2;
  int y = (h - cardH) / 2;
  tft.fillRoundRect(x, y, cardW, cardH, 8, TFT_WHITE);
  tft.drawRoundRect(x, y, cardW, cardH, 8, navy);
  tft.setTextSize(1);
  tft.setTextColor(navy, TFT_WHITE);
  tft.setCursor(x + (cardW - (int)strlen(line1) * 6) / 2, y + 12);
  tft.print(line1);
  tft.setTextColor(muteColor(), TFT_WHITE);
  tft.setCursor(x + (cardW - (int)strlen(line2) * 6) / 2, y + 30);
  tft.print(line2);
}

void drawSectionTitle(int y, const char* title) {
  tft.fillRect(0, y, tft.width(), 16, pageColor());
  tft.fillRect(10, y + 3, 3, 10, navyColor());
  tft.setTextSize(1);
  tft.setTextColor(navyColor(), pageColor());
  tft.setCursor(18, y + 4);
  tft.print(title);
}

bool inputClosed(int pin) {
  int index = findInput(pin);
  if (index < 0) return false;
  bool closed = inputState[index].stable;
  return cfg.inputActiveLow ? closed : !closed;
}

void serviceInputs(unsigned long now) {
  for (int i = 0; i < inputCount; i++) {
    bool next = digitalRead(inputState[i].pin) == LOW;
    if (next != inputState[i].raw) {
      inputState[i].raw = next;
      inputState[i].since = now;
    } else if (now - inputState[i].since >= cfg.debounceMs) {
      inputState[i].stable = inputState[i].raw;
    }
  }
}

void readPressure() {
  for (int i = 0; i < SENSOR_COUNT; i++) {
    int pin = cfg.sensorPin[i];
    if (pin < 0) continue;
    if (netRadioOn && (pin == 12 || pin == 13)) {
      parkRelayPin(pin);
      continue;
    }
    bool relayPin = pin == 12 || pin == 13;
    int samples = relayPin ? 4 : 8;
    uint32_t sum = 0;
    for (int s = 0; s < samples; s++) sum += analogRead(pin);
    if (relayPin) parkRelayPin(pin);
    int raw = (int)(sum / samples);
    float volts = raw * (3.3f / 4095.0f);
    float milliamp = (volts / cfg.shuntOhms) * 1000.0f;
    float span = (milliamp - 4.0f) / 16.0f;
    if (span < 0.0f) span = 0.0f;
    psiLive[i] = span * cfg.sensorSpanMpa[i] * 145.038f * cfg.psiGain;
    milliampLive[i] = milliamp;
    pressureValid[i] = milliamp >= 3.5f && milliamp <= 22.0f && psiLive[i] <= 200.0f;
  }
}

void updateFlow(unsigned long now) {
  if (flowWindowStart == 0) flowWindowStart = now;
  if (now - flowWindowStart < cfg.flowWindowMs) return;
  noInterrupts();
  uint32_t pulses[2] = {flowPulses[0], flowPulses[1]};
  flowPulses[0] = 0;
  flowPulses[1] = 0;
  interrupts();
  float minutes = cfg.flowWindowMs / 60000.0f;
  for (int i = 0; i < 2; i++) {
    float liters = cfg.meter[i].litersPerPulse;
    if (liters <= 0.0f) liters = 0.1f;
    flowLpm[i] = (pulses[i] * liters) / minutes;
  }
  flowWindowStart = now;
}

void serviceFlush(unsigned long now) {
  RuleConfig& rule = cfg.rule[2];
  if (!rule.enabled || rule.mode != MODE_TIMER) {
    flushRunning = false;
    return;
  }
  if (nextFlushAt == 0) nextFlushAt = now + rule.intervalMs;
  if (!flushRunning) {
    if ((long)(now - nextFlushAt) >= 0) {
      flushRunning = true;
      flushStartedAt = now;
      logger("FLUSH", "inicio %lu ms", rule.durationMs);
    }
    return;
  }
  if (now - flushStartedAt >= rule.durationMs) {
    flushRunning = false;
    nextFlushAt = flushStartedAt + rule.intervalMs;
    logger("FLUSH", "fin");
  }
}

bool desiredRelay(int index) {
  RuleConfig& rule = cfg.rule[index];
  if (!rule.enabled || rule.mode == MODE_OFF) return false;
  switch (rule.mode) {
    case MODE_SWITCH:
      return rule.portalOn || inputClosed(rule.inputA);
    case MODE_AND:
      return inputClosed(rule.inputA) && inputClosed(rule.inputB);
    case MODE_TIMER:
      return flushRunning;
    case MODE_FLOW: {
      float lpm = 0.0f;
      if (rule.meterIndex >= 0 && rule.meterIndex < 2) lpm = flowLpm[rule.meterIndex];
      float minOn = rule.minLpm;
      float minOff = rule.minLpm - rule.flowHystLpm;
      if (minOff < 0.0f) minOff = 0.0f;
      if (relayOn[index]) return lpm > minOff || (minOff == 0.0f && lpm > 0.0f);
      return lpm > minOn;
    }
    case MODE_DIFF: {
      int a = rule.sensorA;
      int b = rule.sensorB;
      if (a < 0 || b < 0 || a >= SENSOR_COUNT || b >= SENSOR_COUNT) return false;
      if (!pressureValid[a] || !pressureValid[b]) return false;
      float diff = fabs(psiLive[a] - psiLive[b]);
      float offAt = rule.diffPsi - rule.hystPsi;
      if (offAt < 0.0f) offAt = 0.0f;
      return relayOn[index] ? diff >= offAt : diff >= rule.diffPsi;
    }
    case MODE_PRESSURE: {
      if (!rule.sensorMask || rule.minPsi >= rule.maxPsi) return false;
      for (int s = 0; s < SENSOR_COUNT; s++) {
        if ((rule.sensorMask & (uint8_t)(1 << s)) == 0) continue;
        if (!pressureValid[s]) return false;
        float low = rule.minPsi;
        float high = rule.maxPsi;
        if (relayOn[index]) {
          low -= 1.0f;
          high += 1.0f;
        }
        if (psiLive[s] < low || psiLive[s] > high) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

void applyRelay(int index, bool on) {
  if (relayOn[index] == on) return;
  relayOn[index] = on;
  relayChangedAt[index] = millis();
  writeRelay(cfg.relayPin[index], on);
  logger("RELAY", "%d %s GPIO %d %s", index + 1, RELAY_NAME[index], cfg.relayPin[index], on ? "ON" : "OFF");
}

void serviceRelays(unsigned long now) {
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool want = desiredRelay(i);
    if (want == relayOn[i]) continue;
    bool immediate = i == 0 || i == 2;
    if (immediate || now - relayChangedAt[i] >= cfg.relayDwellMs) applyRelay(i, want);
  }
}

void report() {
  bool diffOk = !netRadioOn && pressureValid[0] && pressureValid[2];
  float diff = diffOk ? fabs(psiLive[0] - psiLive[2]) : -1.0f;
  Serial.println("---");
  for (int i = 0; i < SENSOR_COUNT; i++) {
    int pin = cfg.sensorPin[i];
    if (netRadioOn && (pin == 12 || pin == 13)) {
      Serial.printf("S%d GPIO %d  pausa\n", i + 1, pin);
      continue;
    }
    Serial.printf("S%d GPIO %d  %s  %.2f mA  %.1f psi\n",
                  i + 1, pin, pressureValid[i] ? "ok" : "n/d", milliampLive[i], psiLive[i]);
  }
  if (netRadioOn) Serial.println("WiFi en consulta");
  else if (diff < 0.0f) Serial.println("DIFF 34-13  n/d");
  else Serial.printf("DIFF 34-13  %.1f psi  umbral %.1f\n", diff, cfg.rule[5].diffPsi);
  for (int i = 0; i < 2; i++) {
    char label[12];
    flowSourceLabel(cfg.meter[i], label, sizeof(label));
    bool live = cfg.meter[i].origin == FLOW_PIN && cfg.meter[i].pin >= 0;
    if (live) Serial.printf("F%d %s  %.2f L/min\n", i + 1, label, flowLpm[i]);
    else Serial.printf("F%d %s  n/d\n", i + 1, label);
  }
  Serial.printf("ENTRADAS  16 %s  17 %s  22 %s\n",
                inputClosed(16) ? "ON" : "OFF",
                inputClosed(17) ? "ON" : "OFF",
                inputClosed(22) ? "ON" : "OFF");
  for (int i = 0; i < RELAY_COUNT; i++) {
    Serial.printf("R%d %-8s GPIO %d  %s  %s\n",
                  i + 1, RELAY_NAME[i], cfg.relayPin[i],
                  cfg.rule[i].enabled ? "hab" : "off",
                  relayOn[i] ? "ON" : "OFF");
  }
}

void drawPressureCard(int index) {
  const int gap = 6;
  const int count = PRESSURE_CARDS;
  int w = (tft.width() - gap * (count + 1)) / count;
  int x = gap + index * (w + gap);
  int y = ui.pressureY;
  int h = ui.pressureH;
  bool live = index < SENSOR_COUNT && pressureValid[index];
  paintCard(x, y, w, h, TFT_WHITE, live ? navyColor() : cardLine());
  tft.fillRect(x + 1, y + 8, 3, h - 16, live ? navyColor() : muteColor());

  tft.setTextSize(1);
  tft.setTextColor(muteColor(), TFT_WHITE);
  tft.setCursor(x + 10, y + 6);
  if (index < SENSOR_COUNT) tft.printf("S%d  %d", index + 1, cfg.sensorPin[index]);
  else tft.printf("S%d", index + 1);

  if (index >= SENSOR_COUNT) {
    tft.setTextSize(2);
    tft.setTextColor(muteColor(), TFT_WHITE);
    tft.setCursor(x + 10, y + 26);
    tft.print(index == 4 ? "s/pin" : "n/d");
    return;
  }
  if (!live) {
    tft.setTextSize(2);
    tft.setTextColor(muteColor(), TFT_WHITE);
    tft.setCursor(x + 10, y + 26);
    tft.print("n/d");
    return;
  }
  tft.setTextSize(2);
  tft.setTextColor(inkColor(), TFT_WHITE);
  tft.setCursor(x + 10, y + 22);
  tft.printf("%4.1f", psiLive[index]);
  tft.setTextSize(1);
  tft.setTextColor(muteColor(), TFT_WHITE);
  tft.setCursor(x + 10, y + 42);
  tft.print("psi");
  tft.setCursor(x + 36, y + 42);
  tft.printf("%3.1f", cfg.sensorSpanMpa[index]);
}

void drawWave(int x, int y, uint16_t color) {
  tft.drawLine(x, y, x + 4, y - 3, color);
  tft.drawLine(x + 4, y - 3, x + 8, y, color);
  tft.drawLine(x + 8, y, x + 12, y - 3, color);
  tft.drawLine(x + 12, y - 3, x + 16, y, color);
  tft.drawLine(x, y + 5, x + 4, y + 2, color);
  tft.drawLine(x + 4, y + 2, x + 8, y + 5, color);
  tft.drawLine(x + 8, y + 5, x + 12, y + 2, color);
  tft.drawLine(x + 12, y + 2, x + 16, y + 5, color);
}

void drawFlowCard(int index) {
  const int gap = 8;
  int w = (tft.width() - gap * 3) / 2;
  int x = gap + index * (w + gap);
  int y = ui.flowY;
  bool live = cfg.meter[index].origin == FLOW_PIN && cfg.meter[index].pin >= 0;
  char label[12];
  flowSourceLabel(cfg.meter[index], label, sizeof(label));
  paintCard(x, y, w, ui.flowH, TFT_WHITE, cardLine());
  drawWave(x + 12, y + 20, navyColor());
  tft.setTextSize(1);
  tft.setTextColor(muteColor(), TFT_WHITE);
  tft.setCursor(x + 36, y + 8);
  tft.printf("F%d  %s", index + 1, label);
  tft.setTextSize(2);
  tft.setTextColor(live ? inkColor() : muteColor(), TFT_WHITE);
  tft.setCursor(x + 36, y + 22);
  if (live) tft.printf("%4.1f", flowLpm[index]);
  else tft.print("--");
  tft.setTextSize(1);
  tft.setTextColor(muteColor(), TFT_WHITE);
  tft.setCursor(x + 96, y + 30);
  tft.print("L/min");
}

void drawStatic() {
  if (!displayReady) return;
  tft.fillScreen(pageColor());
  drawHeader();
  drawSectionTitle(ui.pressureTitleY, "PRESION");
  for (int i = 0; i < PRESSURE_CARDS; i++) drawPressureCard(i);
  drawSectionTitle(ui.flowTitleY, "FLUJO");
  drawFlowCard(0);
  drawFlowCard(1);
  drawSectionTitle(ui.relayTitleY, "EQUIPOS");
  drawRelayRow();
  for (int i = 0; i < SENSOR_COUNT; i++) {
    shownPsi[i] = psiLive[i];
    shownValid[i] = pressureValid[i];
  }
  shownLpm[0] = flowLpm[0];
  shownLpm[1] = flowLpm[1];
  learnScreenProbe();
}

uint16_t swapColor(uint16_t color) {
  return (uint16_t)((color >> 8) | (color << 8));
}

bool colorNear(uint16_t a, uint16_t b) {
  int ar = (a >> 11) & 31;
  int ag = (a >> 5) & 63;
  int ab = a & 31;
  int br = (b >> 11) & 31;
  int bg = (b >> 5) & 63;
  int bb = b & 31;
  int dr = ar - br;
  int dg = ag - bg;
  int db = ab - bb;
  if (dr < 0) dr = -dr;
  if (dg < 0) dg = -dg;
  if (db < 0) db = -db;
  return dr <= 8 && dg <= 16 && db <= 8;
}

bool colorMatches(uint16_t got, uint16_t expect) {
  return colorNear(got, expect) || colorNear(got, swapColor(expect));
}

bool pixelBlack(uint16_t color) {
  return ((color >> 11) & 31) < 2 && ((color >> 5) & 63) < 4 && (color & 31) < 2;
}

void learnScreenProbe() {
  uint16_t got = tft.readPixel(2, 2);
  screenReadable = colorMatches(got, navyColor());
  if (!screenReadable && !screenProbeWarned) {
    screenProbeWarned = true;
    logger("CFG", "la pantalla no se puede leer. No hay revision automatica.");
  }
}

bool screenLooksBroken() {
  uint16_t navy = navyColor();
  uint16_t page = pageColor();
  uint16_t headerA = tft.readPixel(2, 2);
  uint16_t headerB = tft.readPixel(6, 6);
  uint16_t headerC = tft.readPixel(8, 4);
  uint16_t paper = tft.readPixel(4, ui.headerH + 2);
  int bad = 0;
  if (pixelBlack(headerA) || !colorMatches(headerA, navy)) bad++;
  if (pixelBlack(headerB) || !colorMatches(headerB, navy)) bad++;
  if (pixelBlack(headerC) || !colorMatches(headerC, navy)) bad++;
  if (pixelBlack(paper) || !colorMatches(paper, page)) bad++;
  return bad >= 2;
}

void repairScreen() {
  logger("CFG", "pantalla negra o pixelada, se redibuja");
  initDisplay();
  computeLayout();
  floodPanel(pageColor());
  shownPump = !relayOn[0];
  shownNet = -2;
  shownSync[0] = 0;
  shownLpm[0] = -999;
  shownLpm[1] = -999;
  for (int i = 0; i < RELAY_COUNT; i++) shownHint[i][0] = 0;
  for (int i = 0; i < SENSOR_COUNT; i++) shownPsi[i] = -999;
  drawStatic();
}

void serviceScreen(unsigned long now) {
  if (!displayReady || netHoldUi || netNeedsRedraw || !screenReadable) return;
  if (nextScreenCheck == 0) nextScreenCheck = now + SCREEN_CHECK_MS;
  if ((long)(now - nextScreenCheck) < 0) return;
  nextScreenCheck = now + SCREEN_CHECK_MS;
  if (screenLooksBroken()) repairScreen();
}

void refreshDisplay() {
  if (!displayReady) return;
  if (netHoldUi) {
    if (!netOverlayReady) {
      drawWaitScreen();
      netOverlayReady = true;
    }
    return;
  }
  if (netNeedsRedraw) {
    static char synced[4500];
    if (netTakeRules(synced, sizeof(synced))) {
      applySyncedRules(synced);
      bindFlowMeters();
    }
    floodPanel(pageColor());
    shownPump = !relayOn[0];
    shownNet = -2;
    shownSync[0] = 0;
    shownLpm[0] = -999;
    shownLpm[1] = -999;
    for (int i = 0; i < RELAY_COUNT; i++) shownHint[i][0] = 0;
    for (int i = 0; i < SENSOR_COUNT; i++) shownPsi[i] = -999;
    drawStatic();
    netNeedsRedraw = false;
    return;
  }
  char liveName[32];
  netCopyName(liveName, sizeof(liveName));
  if (strcmp(shownName, liveName) != 0) drawHeader();
  else drawSyncLine();
  for (int i = 0; i < SENSOR_COUNT; i++) {
    if (shownValid[i] == pressureValid[i] && fabs(shownPsi[i] - psiLive[i]) < 0.1f) continue;
    drawPressureCard(i);
    shownPsi[i] = psiLive[i];
    shownValid[i] = pressureValid[i];
  }
  for (int i = 0; i < 2; i++) {
    if (fabs(shownLpm[i] - flowLpm[i]) < 0.05f) continue;
    drawFlowCard(i);
    shownLpm[i] = flowLpm[i];
  }
  if (ui.relayH < 52) return;
  for (int i = 0; i < RELAY_COUNT; i++) {
    char hint[12];
    relayHint(i, hint, sizeof(hint));
    if (shownRelay[i] == relayOn[i] && strcmp(shownHint[i], hint) == 0) continue;
    int x = 0;
    int w = 0;
    relayCellBox(i, x, w);
    drawEquipCell(x, ui.relayTop, w, ui.relayH, i, relayOn[i]);
  }
}

bool flowPinFree(int pin) {
  if (pin < 0) return false;
  const int blocked[] = {0, 2, 5, 12, 13, 14, 15, 16, 17, 18, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35};
  for (int i = 0; i < 20; i++) {
    if (blocked[i] == pin) return false;
  }
  return true;
}

void copyMeterName(char* dest, const char* src) {
  int i = 0;
  for (; src && src[i] && i < 15; i++) dest[i] = src[i];
  dest[i] = 0;
}

bool switchPinOk(int pin) {
  const int allowed[] = {4, 16, 17, 19, 22};
  for (int i = 0; i < 5; i++) {
    if (allowed[i] == pin) return true;
  }
  return false;
}

bool applySyncedRules(const char* json) {
  DynamicJsonDocument doc(6144);
  if (!json || !json[0] || deserializeJson(doc, json)) return false;
  const char* fetchedName = doc["controller_name"] | "";
  if (!fetchedName[0]) fetchedName = doc["name"] | "";
  if (fetchedName[0]) rememberDeviceName(fetchedName);
  if (doc.containsKey("input_active_low")) cfg.inputActiveLow = doc["input_active_low"].as<bool>();

  JsonArray flows = doc["flows"].as<JsonArray>();
  int flowIndex = 0;
  for (JsonObject flow : flows) {
    if (flowIndex >= 2) break;
    const char* origin = flow["origin"] | "";
    bool fromApi = strcmp(origin, "platform") == 0 || strcmp(origin, "api") == 0;
    const char* name = flow["name"] | "";
    if (!name[0]) name = flow["device_code"] | "";
    copyMeterName(cfg.meter[flowIndex].name, name);
    if (fromApi) {
      cfg.meter[flowIndex].origin = FLOW_API;
      cfg.meter[flowIndex].pin = -1;
    } else if (strcmp(origin, "pin") == 0 || flow.containsKey("pin")) {
      int pin = flow["pin"] | -1;
      bool repeat = flowIndex == 1 && cfg.meter[0].origin == FLOW_PIN && cfg.meter[0].pin == pin;
      if (flowPinFree(pin) && !repeat) {
        cfg.meter[flowIndex].origin = FLOW_PIN;
        cfg.meter[flowIndex].pin = pin;
      } else {
        logger("NET", "medidor %d ignora GPIO %d", flowIndex + 1, pin);
      }
      float liters = flow["liters_per_pulse"] | cfg.meter[flowIndex].litersPerPulse;
      if (liters > 0.0f && liters <= 100.0f) cfg.meter[flowIndex].litersPerPulse = liters;
    }
    flowIndex++;
  }

  for (JsonObject relay : doc["relays"].as<JsonArray>()) {
    int id = relay["id"] | 0;
    if (id < 1 || id > RELAY_COUNT) continue;
    RuleConfig& rule = cfg.rule[id - 1];
    if (relay.containsKey("enabled")) rule.enabled = relay["enabled"].as<bool>();
    const char* mode = relay["mode"] | "";
    if (strcmp(mode, "manual") == 0 || strcmp(mode, "switch") == 0) {
      rule.mode = MODE_SWITCH;
      int pin = relay["input_a"] | rule.inputA;
      if (switchPinOk(pin)) rule.inputA = pin;
      rule.portalOn = relay["on"] | false;
    } else if (strcmp(mode, "and_inputs") == 0) {
      rule.mode = MODE_AND;
      int pinA = relay["input_a"] | rule.inputA;
      int pinB = relay["input_b"] | rule.inputB;
      if (switchPinOk(pinA)) rule.inputA = pinA;
      if (switchPinOk(pinB)) rule.inputB = pinB;
    } else if (strcmp(mode, "timer") == 0) {
      rule.mode = MODE_TIMER;
      unsigned long interval = relay["interval_ms"] | rule.intervalMs;
      unsigned long duration = relay["duration_ms"] | rule.durationMs;
      if (interval < 1000) interval = 1000;
      if (duration < 1000) duration = 1000;
      if (duration >= interval) duration = interval / 2;
      rule.intervalMs = interval;
      rule.durationMs = duration;
      if (id == 3 && !flushRunning) nextFlushAt = millis() + interval;
    } else if (strcmp(mode, "flow") == 0) {
      rule.mode = MODE_FLOW;
      int meter = relay["meter"] | rule.meterIndex;
      if (meter < 0 || meter > 1) meter = 0;
      rule.meterIndex = meter;
      float minLpm = relay["min_lpm"] | rule.minLpm;
      if (minLpm < 0.0f) minLpm = 0.0f;
      rule.minLpm = minLpm;
      float hyst = relay["hyst_lpm"] | rule.flowHystLpm;
      if (hyst < 0.0f) hyst = 0.0f;
      rule.flowHystLpm = hyst;
    } else if (strcmp(mode, "diff") == 0) {
      rule.mode = MODE_DIFF;
      int sensorA = relay["sensor_a"] | rule.sensorA;
      int sensorB = relay["sensor_b"] | rule.sensorB;
      if (sensorA >= 0 && sensorA < SENSOR_COUNT) rule.sensorA = sensorA;
      if (sensorB >= 0 && sensorB < SENSOR_COUNT && sensorB != rule.sensorA) rule.sensorB = sensorB;
      float diffPsi = relay["diff_psi"] | rule.diffPsi;
      float hystPsi = relay["hyst_psi"] | rule.hystPsi;
      if (diffPsi >= 0.1f) rule.diffPsi = diffPsi;
      if (hystPsi >= 0.0f && hystPsi < rule.diffPsi) rule.hystPsi = hystPsi;
    } else if (strcmp(mode, "pressure") == 0) {
      rule.mode = MODE_PRESSURE;
      rule.portalOn = false;
      float minPsi = relay["min_psi"] | rule.minPsi;
      float maxPsi = relay["max_psi"] | rule.maxPsi;
      if (minPsi >= 0.0f) rule.minPsi = minPsi;
      if (maxPsi > rule.minPsi) rule.maxPsi = maxPsi;
      uint8_t mask = 0;
      JsonArray sensors = relay["sensors"].as<JsonArray>();
      for (JsonVariant sensor : sensors) {
        int index = sensor.as<int>();
        if (index >= 0 && index < SENSOR_COUNT) mask |= (uint8_t)(1 << index);
      }
      rule.sensorMask = mask;
    } else if (mode[0]) {
      logger("NET", "relevador %d modo %s no reconocido", id, mode);
    }
    Serial.printf("[NET] R%d %s %s\n", id, mode[0] ? mode : "local", rule.enabled ? "hab" : "off");
    addInput(rule.inputA);
    addInput(rule.inputB);
  }
  logger("NET", "reglas aplicadas");
  netUpdatedAt = millis();
  time_t now = time(nullptr);
  if (now > 1700000000) netUpdatedClock = now;
  return true;
}

void setup() {
  Serial.begin(115200);
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  initOutputs();
  if (configPinsConflict()) {
    logger("CFG", "hay pines repetidos o el flush dura mas que su periodo");
  }
  analogReadResolution(12);
  for (int i = 0; i < SENSOR_COUNT; i++) {
    if (cfg.sensorPin[i] < 0) continue;
    analogSetPinAttenuation(cfg.sensorPin[i], ADC_11db);
  }
  initInputs();
  initDisplay();
  computeLayout();
  nextFlushAt = millis() + cfg.rule[2].intervalMs;
  flowWindowStart = millis();
  drawStatic();
  logger("CFG", "listo. Consulta cada %lu s. S1-S4 con la radio apagada.", NET_SYNC_MS / 1000);
  tokenStart(NET_API_HOST, NET_LOGIN_EMAIL, NET_LOGIN_PASSWORD, NET_DEVICE_ID, NET_SYNC_MS);
}

void loop() {
  unsigned long now = millis();
  serviceInputs(now);
  if (now - lastRead >= cfg.readMs) {
    readPressure();
    lastRead = now;
  }
  updateFlow(now);
  serviceFlush(now);
  serviceRelays(now);
  if (now - lastReport >= cfg.reportMs) {
    report();
    lastReport = now;
  }
  refreshDisplay();
  serviceScreen(now);
}
