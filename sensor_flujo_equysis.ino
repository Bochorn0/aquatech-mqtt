// ================== EQUYSIS — LECTOR OUTPUT GPIO 19 ==================
// Cableado: Verde OUTPUT → GPIO 19 | Azul GND → GND
//
// Del dump bruto:
//   - NO es UART. Los 00 eran pulsos de ~5.09 ms vistos como serial.
//   - Pulso real: LOW ~5088-5105 us, idle HIGH.
//   - 0.00574 → 0.00750 m3 ≈ 18 pulsos → 0.0001 m3/pulso (0.1 L).
//
// Pon M3_INICIAL al valor del LCD al cargar. Serial Monitor 115200.

const uint32_t BAUD = 115200;
const int PIN_METER_OUTPUT = 19;
const uint32_t REPORT_MS = 5000;
const uint32_t BOOT_IGNORE_MS = 1500;

// Pulso valido: LOW entre 2 ms y 15 ms (el real dura ~5.09 ms).
const uint32_t PULSE_MIN_US = 2000;
const uint32_t PULSE_MAX_US = 15000;

// Lectura del LCD al subir el sketch.
const double M3_INICIAL = 0.00750;
const double M3_POR_PULSO = 0.0001;
const int DECIMALES = 5;

volatile uint32_t pulseCount = 0;
volatile uint32_t rejectedCount = 0;
volatile uint32_t lastLowUs = 0;
volatile uint32_t lastHighUs = 0;
volatile uint32_t fallUs = 0;
volatile uint32_t minLowUs = 0xFFFFFFFF;
volatile uint32_t maxLowUs = 0;
volatile int irqLevel = HIGH;

uint32_t lastCount = 0;
uint32_t n = 0;

void IRAM_ATTR onEdge() {
  const uint32_t now = micros();
  const int level = digitalRead(PIN_METER_OUTPUT);

  if (irqLevel == HIGH && level == LOW) {
    lastHighUs = now - fallUs;
    fallUs = now;
  } else if (irqLevel == LOW && level == HIGH) {
    const uint32_t width = now - fallUs;
    lastLowUs = width;
    if (width >= PULSE_MIN_US && width <= PULSE_MAX_US) {
      pulseCount++;
      if (width < minLowUs) minLowUs = width;
      if (width > maxLowUs) maxLowUs = width;
    } else {
      rejectedCount++;
    }
  }
  irqLevel = level;
}

void setup() {
  Serial.begin(BAUD);
  pinMode(PIN_METER_OUTPUT, INPUT_PULLUP);
  delay(BOOT_IGNORE_MS);

  irqLevel = digitalRead(PIN_METER_OUTPUT);
  fallUs = micros();
  pulseCount = 0;
  rejectedCount = 0;
  lastCount = 0;
  attachInterrupt(digitalPinToInterrupt(PIN_METER_OUTPUT), onEdge, CHANGE);
}

void loop() {
  delay(REPORT_MS);
  n++;

  noInterrupts();
  const uint32_t count = pulseCount;
  const uint32_t rejected = rejectedCount;
  const uint32_t lowUs = lastLowUs;
  const uint32_t highUs = lastHighUs;
  const uint32_t minLow = (minLowUs == 0xFFFFFFFF) ? 0 : minLowUs;
  const uint32_t maxLow = maxLowUs;
  interrupts();

  const uint32_t delta = count - lastCount;
  lastCount = count;

  const double m3DesdeBoot = (double)count * M3_POR_PULSO;
  const double m3Medidor = M3_INICIAL + m3DesdeBoot;
  const double m3Ventana = (double)delta * M3_POR_PULSO;
  const double litrosMedidor = m3Medidor * 1000.0;
  const double litrosVentana = m3Ventana * 1000.0;
  const double litrosPorMin = litrosVentana * (60000.0 / REPORT_MS);

  Serial.println("==========================================");
  Serial.println("LECTOR EQUYSIS OUTPUT");
  Serial.print("n=");
  Serial.print(n);
  Serial.print("  millis=");
  Serial.println(millis());
  Serial.print("GPIO 19: ");
  Serial.println(digitalRead(PIN_METER_OUTPUT) ? "HIGH" : "LOW");

  Serial.println("--- RAW ---");
  Serial.print("pulsos_total=");
  Serial.println(count);
  Serial.print("pulsos_5s=");
  Serial.println(delta);
  Serial.print("pulsos_rechazados=");
  Serial.println(rejected);
  Serial.print("ultimo_pulso_low_us=");
  Serial.println(lowUs);
  Serial.print("ultimo_idle_high_us=");
  Serial.println(highUs);
  Serial.print("min_pulso_us=");
  Serial.println(minLow);
  Serial.print("max_pulso_us=");
  Serial.println(maxLow);

  Serial.println("--- CONVERSION ---");
  Serial.print("m3_por_pulso=");
  Serial.println(M3_POR_PULSO, DECIMALES);
  Serial.print("m3_inicial_lcd=");
  Serial.println(M3_INICIAL, DECIMALES);
  Serial.print("m3_medidor=");
  Serial.println(m3Medidor, DECIMALES);
  Serial.print("m3_desde_boot=");
  Serial.println(m3DesdeBoot, DECIMALES);
  Serial.print("m3_5s=");
  Serial.println(m3Ventana, DECIMALES);
  Serial.print("litros_medidor=");
  Serial.println(litrosMedidor, DECIMALES);
  Serial.print("litros_5s=");
  Serial.println(litrosVentana, DECIMALES);
  Serial.print("L/min=");
  Serial.println(litrosPorMin, DECIMALES);
  Serial.println("==========================================");
  Serial.println();
}
