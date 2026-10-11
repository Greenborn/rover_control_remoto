/*
 * ESP8266 - Access Point + control de motores (puente H L298N vía PCF8574)
 *
 * Al encender, el módulo crea su propia red WiFi (modo Access Point).
 * Cualquier dispositivo que se conecte a esa red puede abrir
 * http://192.168.4.1/ en el navegador y manejar el rover con un D-pad.
 *
 * Los pines IN1-IN4 del L298N ya no se manejan por GPIO directo: se manejan
 * a través de un expansor de I/O PCF8574 por el bus I2C (SDA=D2, SCL=D1).
 *
 * Sin cámara: este sketch es la versión "solo motores" para un ESP8266
 * separado del ESP32-CAM (ver ../esp32cam_wifi_ap_stream).
 *
 * Placa: NodeMCU / Wemos D1 Mini (ESP8266, módulo ESP-12E).
 * Board manager: "NodeMCU 1.0 (ESP-12E Module)" o "LOLIN(WEMOS) D1 R2 & mini"
 * (paquete esp8266 de la comunidad Arduino) - ambas usan el mismo mapeo D1..D8.
 *
 * Librerías usadas: todas incluidas en el core esp8266 de Arduino
 * (ESP8266WiFi.h, ESP8266WebServer.h, Wire.h) - no requiere instalar nada extra.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>

// ---------------------------------------------------------------------------
// Configuración de la red WiFi que genera el ESP8266
// ---------------------------------------------------------------------------
const char *AP_SSID     = "rover-tito";    // Nombre de la red WiFi
const char *AP_PASSWORD = "rover1234";     // Mínimo 8 caracteres. Poner "" para red abierta.
const int   AP_CHANNEL  = 1;               // Canal WiFi (1-13)
const int   AP_MAX_CONN = 4;               // Cantidad máxima de clientes conectados

// IP fija del AP (por defecto el ESP8266 usa 192.168.4.1, se define explícito)
IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ---------------------------------------------------------------------------
// Pines de motores - puente H L298N (tracción diferencial), manejado a
// través de un expansor de I/O PCF8574 conectado por I2C.
// ENA/ENB del L298N están puenteados a 5V (siempre habilitados), por eso
// alcanza con manejar IN1/IN2 de cada motor a velocidad fija, sin PWM.
//
// Bus I2C: SDA=D2 (GPIO4), SCL=D1 (GPIO5) - pines estándar del core esp8266
// (NodeMCU/Wemos D1 Mini). El PCF8574 se conecta ahí, no a GPIOs directos.
//
// Dirección I2C: depende de cómo estén puenteados A0/A1/A2 en el módulo, y
// varía según el diseño de la placa (hay módulos donde "jumper abierto" es
// nivel alto y otros donde es nivel bajo). En vez de hardcodear una sola
// dirección, el firmware escanea todas las direcciones posibles de PCF8574
// (0x20-0x27) y PCF8574A (0x38-0x3F) al arrancar y usa la primera que
// responda. pcf8574Addr queda en 0 si no se encontró ninguna.
const uint8_t PCF8574_CANDIDATE_ADDRS[] = {
  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
  0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
};
const uint8_t PCF8574_CANDIDATE_COUNT =
    sizeof(PCF8574_CANDIDATE_ADDRS) / sizeof(PCF8574_CANDIDATE_ADDRS[0]);
uint8_t pcf8574Addr = 0x00; // dirección detectada (0 = ninguna)

// Bits de salida del PCF8574 (P0..P7) para cada IN del L298N. Motores con
// polaridad invertida (IN1/IN2 cruzados) para que giren en el sentido físico
// correcto al avanzar/retroceder.
#define MOTOR_IZQ_IN1   0   // P0
#define MOTOR_IZQ_IN2   1   // P1
#define MOTOR_DER_IN1   2   // P2
#define MOTOR_DER_IN2   4   // P4 (P3 sin conectar)

enum MotorDir { MOTOR_STOP, MOTOR_FORWARD, MOTOR_BACKWARD };

// Estado actual de las 8 salidas del PCF8574 (se escribe entero por I2C en
// cada cambio porque el chip no permite escribir un solo pin a la vez).
uint8_t pcf8574State = 0x00;

static void pcf8574Write(uint8_t value) {
  pcf8574State = value;
  if (pcf8574Addr == 0x00) return; // nada detectado, no escribir a ciegas
  Wire.beginTransmission(pcf8574Addr);
  Wire.write(pcf8574State);
  Wire.endTransmission();
}

// Prende/apaga un solo bit (P0-P3) sin tocar el resto - lo usa el modo
// diagnóstico del portal web para probar cada salida por separado.
static void pcf8574SetBit(uint8_t bit, bool on) {
  uint8_t out = pcf8574State;
  if (on) out |= (1 << bit); else out &= ~(1 << bit);
  pcf8574Write(out);
}

// Detecta si el PCF8574 responde en el bus I2C (ACK a su dirección), sin
// tocar el estado de las salidas. Se usa para avisar en el portal web si el
// expansor está desconectado o mal cableado.
bool pcf8574Detected = false;
#define I2C_CHECK_INTERVAL_MS 1000
unsigned long lastI2cCheckMs = 0;

// Recorre PCF8574_CANDIDATE_ADDRS y devuelve la primera dirección que
// responda (ACK), o 0x00 si ninguna responde.
static uint8_t pcf8574Scan() {
  for (uint8_t i = 0; i < PCF8574_CANDIDATE_COUNT; i++) {
    uint8_t addr = PCF8574_CANDIDATE_ADDRS[i];
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return addr;
  }
  return 0x00;
}

// Reconfirma la dirección ya detectada (rápido); si no hay ninguna, vuelve a
// escanear todo el rango de candidatas (para detectar una conexión en
// caliente).
static bool pcf8574Probe() {
  if (pcf8574Addr != 0x00) {
    Wire.beginTransmission(pcf8574Addr);
    if (Wire.endTransmission() == 0) return true;
  }
  pcf8574Addr = pcf8574Scan();
  return pcf8574Addr != 0x00;
}

// Watchdog de movimiento: si no llega un comando de /move en este tiempo
// (por ejemplo por un corte de WiFi) se frenan los motores solos.
#define MOVE_TIMEOUT_MS 800
unsigned long lastMoveCmdMs = 0;
bool motorsRunning = false;

ESP8266WebServer server(80);

// ---------------------------------------------------------------------------
// Página HTML principal (D-pad de movimiento)
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>rover-tito</title>
<style>
  * {
    box-sizing: border-box;
    user-select: none;
    -webkit-user-select: none;
    -webkit-touch-callout: none;
  }
  body {
    margin: 0;
    background: #10151a;
    color: #e8edf2;
    font-family: -apple-system, Segoe UI, Roboto, Arial, sans-serif;
    display: flex;
    flex-direction: column;
    align-items: center;
    min-height: 100vh;
    -webkit-tap-highlight-color: transparent;
  }
  header {
    width: 100%;
    padding: 14px 0;
    text-align: center;
    background: #0b0f13;
    border-bottom: 1px solid #1f2a33;
  }
  header h1 {
    margin: 0;
    font-size: 1.2rem;
    letter-spacing: 1px;
  }
  .status {
    font-size: 0.75rem;
    color: #7fdc8c;
    margin-top: 4px;
  }
  .signal {
    display: flex;
    align-items: flex-end;
    justify-content: center;
    gap: 2px;
    margin-top: 6px;
  }
  .signal-bar {
    width: 5px;
    border-radius: 1px;
    background: #334351;
  }
  .signal-bar.on { background: #7fdc8c; }
  .signal-bar.on.warn { background: #e0b84a; }
  .signal-bar.on.bad { background: #e05a5a; }
  .signal-bar:nth-child(1) { height: 6px; }
  .signal-bar:nth-child(2) { height: 10px; }
  .signal-bar:nth-child(3) { height: 14px; }
  .signal-bar:nth-child(4) { height: 18px; }
  .signal-ms {
    font-size: 0.65rem;
    color: #7a8a97;
    margin-left: 4px;
    align-self: center;
  }
  .warning {
    display: none;
    width: 100%;
    box-sizing: border-box;
    padding: 8px 12px;
    background: #4a1f1f;
    color: #ffb3b3;
    border-bottom: 1px solid #7a2e2e;
    text-align: center;
    font-size: 0.8rem;
  }
  .warning.show { display: block; }
  button {
    background: #1f2a33;
    color: #e8edf2;
    border: 1px solid #334351;
    border-radius: 8px;
    padding: 10px 16px;
    font-size: 0.9rem;
    cursor: pointer;
    touch-action: none;
  }
  button:active { background: #334351; }

  .dpad-wrap {
    margin: 40px 0 30px;
    padding: 0 12px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
  }
  .dpad-title {
    font-size: 0.75rem;
    color: #7a8a97;
    letter-spacing: 1px;
  }
  .dpad {
    display: grid;
    grid-template-columns: 72px 72px 72px;
    grid-template-rows: 72px 72px 72px;
    gap: 8px;
  }
  .dpad-btn {
    font-size: 1.6rem;
    padding: 0;
    touch-action: none;
    user-select: none;
    -webkit-user-select: none;
  }
  .dpad-btn:active, .dpad-btn.pressed { background: #3a6ea5; border-color: #3a6ea5; }
  .dpad-up    { grid-area: 1 / 2 / 2 / 3; }
  .dpad-left  { grid-area: 2 / 1 / 3 / 2; }
  .dpad-stop  { grid-area: 2 / 2 / 3 / 3; }
  .dpad-right { grid-area: 2 / 3 / 3 / 4; }
  .dpad-down  { grid-area: 3 / 2 / 4 / 3; }

  .diag-wrap {
    margin: 0 0 30px;
    padding: 0 12px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 8px;
  }
  .diag-row {
    display: flex;
    gap: 8px;
  }
  .diag-btn {
    font-size: 0.85rem;
    border-color: #4a3a1f;
  }
  .diag-btn:active, .diag-btn.pressed { background: #a56e1f; border-color: #a56e1f; }
</style>
</head>
<body>
  <div class="warning" id="pcfWarning">⚠ Expansor PCF8574 no detectado — revisar cableado I2C (SDA/SCL) y alimentación</div>
  <header>
    <h1>rover-tito</h1>
    <div class="status" id="status">conectando...</div>
    <div class="signal" id="signal" title="calidad de señal WiFi (estimada por latencia, el ESP8266 no reporta RSSI de clientes en modo AP)">
      <span class="signal-bar"></span>
      <span class="signal-bar"></span>
      <span class="signal-bar"></span>
      <span class="signal-bar"></span>
      <span class="signal-ms" id="signalMs"></span>
    </div>
  </header>

  <div class="dpad-wrap">
    <div class="dpad-title">MOVIMIENTO</div>
    <div class="dpad">
      <button class="dpad-btn dpad-up"    data-dir="forward">▲</button>
      <button class="dpad-btn dpad-left"  data-dir="left">◀</button>
      <button class="dpad-btn dpad-stop"  data-dir="stop">■</button>
      <button class="dpad-btn dpad-right" data-dir="right">▶</button>
      <button class="dpad-btn dpad-down"  data-dir="backward">▼</button>
    </div>
  </div>

  <div class="diag-wrap">
    <div class="dpad-title">DIAGNÓSTICO - SALIDAS INDIVIDUALES (PCF8574)</div>
    <div class="diag-row">
      <button class="diag-btn" data-pin="0">IN1</button>
      <button class="diag-btn" data-pin="1">IN2</button>
      <button class="diag-btn" data-pin="2">IN3</button>
      <button class="diag-btn" data-pin="4">IN4</button>
    </div>
  </div>

<script>
  const statusEl = document.getElementById('status');

  // D-pad de movimiento: al presionar se manda la dirección y se reenvía
  // cada 400ms mientras se mantiene (keepalive) para sostener el watchdog
  // de seguridad del firmware (frena solo si deja de recibir comandos).
  let currentDir = 'stop';
  let keepAliveTimer = null;

  function sendMove(dir) {
    fetch('/move?dir=' + dir)
      .then(() => { statusEl.textContent = 'conectado'; })
      .catch(() => { statusEl.textContent = 'sin conexión'; });
  }

  function startMove(dir) {
    if (dir === 'stop' || dir === currentDir) return;
    currentDir = dir;
    sendMove(dir);
    clearInterval(keepAliveTimer);
    keepAliveTimer = setInterval(() => sendMove(currentDir), 400);
  }

  function stopMove() {
    clearInterval(keepAliveTimer);
    keepAliveTimer = null;
    if (currentDir === 'stop') return;
    currentDir = 'stop';
    sendMove('stop');
  }

  document.querySelectorAll('.dpad-btn').forEach((btn) => {
    const dir = btn.dataset.dir;
    if (dir === 'stop') {
      btn.addEventListener('click', stopMove);
      return;
    }
    btn.addEventListener('pointerdown', (e) => { e.preventDefault(); startMove(dir); });
    btn.addEventListener('pointerup', stopMove);
    btn.addEventListener('pointerleave', stopMove);
    btn.addEventListener('pointercancel', stopMove);
  });

  // Teclado (flechas / WASD) para manejar desde PC sin tocar la pantalla.
  const KEY_DIR = {
    ArrowUp: 'forward', w: 'forward', W: 'forward',
    ArrowDown: 'backward', s: 'backward', S: 'backward',
    ArrowLeft: 'left', a: 'left', A: 'left',
    ArrowRight: 'right', d: 'right', D: 'right'
  };
  window.addEventListener('keydown', (e) => {
    if (KEY_DIR[e.key]) startMove(KEY_DIR[e.key]);
  });
  window.addEventListener('keyup', (e) => {
    if (KEY_DIR[e.key]) stopMove();
  });

  // Botones de diagnóstico: activan una sola salida del PCF8574 mientras se
  // mantienen presionados (press-and-hold), igual que el D-pad, con keepalive
  // para sostener el watchdog de seguridad del firmware.
  let currentTestPin = null;
  let testKeepAliveTimer = null;

  function sendTestPin(pin, on) {
    fetch('/testPin?pin=' + pin + '&state=' + (on ? 'on' : 'off')).catch(() => {});
  }

  function startTest(pin) {
    if (currentTestPin === pin) return;
    currentTestPin = pin;
    sendTestPin(pin, true);
    clearInterval(testKeepAliveTimer);
    testKeepAliveTimer = setInterval(() => sendTestPin(pin, true), 400);
  }

  function stopTest() {
    clearInterval(testKeepAliveTimer);
    testKeepAliveTimer = null;
    if (currentTestPin === null) return;
    sendTestPin(currentTestPin, false);
    currentTestPin = null;
  }

  document.querySelectorAll('.diag-btn').forEach((btn) => {
    const pin = parseInt(btn.dataset.pin, 10);
    btn.addEventListener('pointerdown', (e) => { e.preventDefault(); startTest(pin); });
    btn.addEventListener('pointerup', stopTest);
    btn.addEventListener('pointerleave', stopTest);
    btn.addEventListener('pointercancel', stopTest);
  });

  // Confirma la conexión al cargar la página (y deja los motores detenidos).
  sendMove('stop');

  // Avisa si el expansor PCF8574 no responde por I2C (cableado/alimentación).
  const pcfWarningEl = document.getElementById('pcfWarning');

  // Medidor de señal WiFi: el ESP8266 en modo Access Point no expone el RSSI
  // de los clientes conectados (el SDK no lo reporta para el softAP), así
  // que se estima la calidad de la conexión por la latencia (round-trip) del
  // propio polling de /status. Es una aproximación razonable: WiFi débil
  // produce respuestas más lentas o fallidas antes de perderse del todo.
  const signalBars = document.querySelectorAll('#signal .signal-bar');
  const signalMsEl = document.getElementById('signalMs');

  function setSignal(bars, cls, label) {
    signalBars.forEach((bar, i) => {
      bar.classList.toggle('on', i < bars);
      bar.classList.remove('warn', 'bad');
      if (i < bars && cls) bar.classList.add(cls);
    });
    signalMsEl.textContent = label;
  }

  function pollStatus() {
    const start = performance.now();
    fetch('/status')
      .then((r) => r.json())
      .then((data) => {
        const rtt = Math.round(performance.now() - start);
        pcfWarningEl.classList.toggle('show', !data.pcf8574);
        if (rtt < 100) setSignal(4, null, rtt + 'ms');
        else if (rtt < 250) setSignal(3, null, rtt + 'ms');
        else if (rtt < 600) setSignal(2, 'warn', rtt + 'ms');
        else setSignal(1, 'bad', rtt + 'ms');
      })
      .catch(() => { setSignal(0, 'bad', 'sin señal'); });
  }
  pollStatus();
  setInterval(pollStatus, 1000);
</script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------
// Control de motores (puente H L298N vía PCF8574) - tracción diferencial
// ---------------------------------------------------------------------------
// Prepara en "out" los bits de in1/in2 para la dirección pedida (sin escribir
// todavía por I2C, para poder mandar las 4 salidas en una sola transacción).
static void setMotor(uint8_t &out, int in1, int in2, MotorDir dir) {
  if (dir == MOTOR_FORWARD)  out |= (1 << in1); else out &= ~(1 << in1);
  if (dir == MOTOR_BACKWARD) out |= (1 << in2); else out &= ~(1 << in2);
}

// dir: "forward" | "backward" | "left" | "right" | "stop"
// Para girar en el lugar, una rueda va adelante y la otra atrás.
static void driveRover(const char *dir) {
  uint8_t out = pcf8574State;

  if (strcmp(dir, "forward") == 0) {
    setMotor(out, MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_FORWARD);
    setMotor(out, MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_FORWARD);
  } else if (strcmp(dir, "backward") == 0) {
    setMotor(out, MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_BACKWARD);
    setMotor(out, MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_BACKWARD);
  } else if (strcmp(dir, "left") == 0) {
    setMotor(out, MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_BACKWARD);
    setMotor(out, MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_FORWARD);
  } else if (strcmp(dir, "right") == 0) {
    setMotor(out, MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_FORWARD);
    setMotor(out, MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_BACKWARD);
  } else {
    setMotor(out, MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_STOP);
    setMotor(out, MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_STOP);
  }

  pcf8574Write(out);
}

// Autotesteo al iniciar: activa cada una de las 4 salidas (IN1-IN4) de forma
// secuencial, 250ms cada una y el resto apagadas, para verificar a simple
// vista (motor que se mueve / led del L298N) que cada canal responde.
// No mueve nada si no se detectó el PCF8574 en el bus I2C.
static void pcf8574SelfTest() {
  if (!pcf8574Detected) {
    Serial.println("Autotesteo omitido: PCF8574 no detectado.");
    return;
  }

  struct { uint8_t bit; const char *label; } outputs[] = {
    { MOTOR_IZQ_IN1, "motor izquierdo IN1 (P0)" },
    { MOTOR_IZQ_IN2, "motor izquierdo IN2 (P1)" },
    { MOTOR_DER_IN1, "motor derecho IN3 (P2)" },
    { MOTOR_DER_IN2, "motor derecho IN4 (P4)" },
  };

  Serial.println("Autotesteo de salidas (PCF8574): activando cada IN por 250ms...");
  for (uint8_t i = 0; i < 4; i++) {
    Serial.printf("  -> %s\n", outputs[i].label);
    pcf8574Write(1 << outputs[i].bit);
    delay(250);
  }
  pcf8574Write(0x00);
  Serial.println("Autotesteo terminado.");
}

// ---------------------------------------------------------------------------
// Handlers HTTP
// ---------------------------------------------------------------------------
void handleIndex() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// /move?dir=forward|backward|left|right|stop
void handleMove() {
  String dir = server.hasArg("dir") ? server.arg("dir") : "stop";

  driveRover(dir.c_str());
  motorsRunning = (dir != "stop");
  lastMoveCmdMs = millis();

  server.send(200, "text/plain", "ok");
}

// /testPin?pin=0-7&state=on|off -> diagnóstico: activa una sola salida del
// PCF8574 (P0, P1, P2, P4, las usadas por IN1-IN4) sin pasar por la lógica de
// dirección de driveRover(). Usa el mismo watchdog que /move.
void handleTestPin() {
  if (!server.hasArg("pin")) {
    server.send(400, "text/plain", "falta pin");
    return;
  }
  int pin = server.arg("pin").toInt();
  if (pin < 0 || pin > 7) {
    server.send(400, "text/plain", "pin invalido (0-7)");
    return;
  }
  bool on = server.hasArg("state") && server.arg("state") == "on";

  pcf8574SetBit(pin, on);
  motorsRunning = (pcf8574State != 0x00);
  lastMoveCmdMs = millis();

  server.send(200, "text/plain", "ok");
}

// /status -> estado del expansor PCF8574, para que la página web avise si
// no está conectado/respondiendo por I2C.
void handleStatus() {
  char addrHex[8];
  snprintf(addrHex, sizeof(addrHex), "0x%02X", pcf8574Addr);
  String json = String("{\"pcf8574\":") + (pcf8574Detected ? "true" : "false") +
                ",\"pcf8574Addr\":\"" + addrHex + "\"}";
  server.send(200, "application/json", json);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  Wire.begin(D2, D1); // SDA, SCL -> PCF8574
  pcf8574Detected = pcf8574Probe();
  if (pcf8574Detected) {
    Serial.printf("PCF8574 detectado en dirección I2C 0x%02X\n", pcf8574Addr);
  } else {
    Serial.println("AVISO: no se detecta el PCF8574 en el bus I2C (revisar cableado/alimentación).");
  }
  pcf8574SelfTest();
  driveRover("stop"); // motores detenidos al arrancar

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);

  Serial.println();
  Serial.println("Red WiFi creada:");
  Serial.printf("  SSID:     %s\n", AP_SSID);
  Serial.printf("  Password: %s\n", AP_PASSWORD);
  Serial.printf("  IP:       http://%s/\n", WiFi.softAPIP().toString().c_str());

  server.on("/", handleIndex);
  server.on("/move", handleMove);
  server.on("/testPin", handleTestPin);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("Servidor web listo.");
}

void loop() {
  server.handleClient();

  // Watchdog de movimiento: si dejan de llegar comandos de /move (por un
  // corte de WiFi, por ejemplo) se frenan los motores solos.
  if (motorsRunning && (millis() - lastMoveCmdMs > MOVE_TIMEOUT_MS)) {
    driveRover("stop");
    motorsRunning = false;
    Serial.println("Motores detenidos por timeout de seguridad (sin señal).");
  }

  // Re-chequea periódicamente si el PCF8574 responde por I2C, para que el
  // portal web pueda avisar si se desconecta/reconecta en caliente.
  if (millis() - lastI2cCheckMs > I2C_CHECK_INTERVAL_MS) {
    lastI2cCheckMs = millis();
    pcf8574Detected = pcf8574Probe();
  }
}
