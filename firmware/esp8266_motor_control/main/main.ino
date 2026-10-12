/*
 * ESP8266 - Access Point + control de motores (puente H L298N vía PCF8574)
 *
 * Al encender, el módulo crea su propia red WiFi (modo Access Point).
 * Cualquier dispositivo que se conecte a esa red puede abrir
 * http://192.168.4.1/ en el navegador y manejar el rover con un joystick
 * virtual (como en un juego de carreras) o con el teclado (adelante/atrás +
 * izquierda/derecha para trazar curvas abiertas, no giros en el lugar).
 *
 * Los pines IN1-IN4 del L298N ya no se manejan por GPIO directo: se manejan
 * a través de un expansor de I/O PCF8574 por el bus I2C (SDA=D2, SCL=D1).
 *
 * ENA/ENB del L298N están puenteados a 5V (siempre habilitados), así que no
 * hay PWM de hardware para variar la velocidad. Para poder tener velocidad
 * progresiva por rueda (y así trazar curvas abiertas en vez de pivotar en el
 * lugar), el firmware hace PWM por software: prende y apaga IN1/IN2 de cada
 * rueda a baja frecuencia (ver updateMotorPWM()) con el duty cycle pedido
 * desde la web.
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
// ENA/ENB del L298N están puenteados a 5V (siempre habilitados): no hay PWM
// de hardware, así que la velocidad progresiva de cada rueda se logra
// prendiendo y apagando IN1/IN2 por software (ver updateMotorPWM()).
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

// Watchdog de movimiento: si no llega un comando de /drive en este tiempo
// (por ejemplo por un corte de WiFi, o al alejarse del alcance del AP) se
// frenan los motores solos. El keepalive del navegador manda /drive cada
// 150ms mientras hay movimiento (ver sendDrive() en INDEX_HTML), así que
// 500ms da margen de sobra para jitter normal sin tardar en frenar si se
// pierde el enlace.
#define MOVE_TIMEOUT_MS 500
unsigned long lastMoveCmdMs = 0;
bool motorsRunning = false;

// Mientras se mantiene presionado un botón de diagnóstico (/testPin),
// updateMotorPWM() no debe tocar el PCF8574: si no, el PWM de las ruedas
// (a 0% porque no hay joystick/teclado activo) pisaría el bit de prueba en
// la siguiente vuelta de loop().
bool diagActive = false;

ESP8266WebServer server(80);

// ---------------------------------------------------------------------------
// Página HTML principal (joystick virtual + control por teclado)
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
    position: relative;
  }
  header h1 {
    margin: 0;
    font-size: 1.2rem;
    letter-spacing: 1px;
  }
  .settings-btn {
    position: absolute;
    top: 10px;
    right: 12px;
    padding: 6px 10px;
    font-size: 1rem;
    line-height: 1;
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

  .joy-wrap {
    margin: 40px 0 30px;
    padding: 0 12px;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 10px;
  }
  .joy-title {
    font-size: 0.75rem;
    color: #7a8a97;
    letter-spacing: 1px;
  }
  .joy-hint {
    font-size: 0.7rem;
    color: #556573;
    text-align: center;
    max-width: 260px;
  }
  .joystick {
    width: 190px;
    height: 190px;
    border-radius: 50%;
    background: #1f2a33;
    border: 1px solid #334351;
    position: relative;
    touch-action: none;
  }
  .joystick-knob {
    width: 68px;
    height: 68px;
    border-radius: 50%;
    background: #3a6ea5;
    border: 1px solid #5a8ec5;
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    touch-action: none;
    pointer-events: none;
  }
  .joystick.active .joystick-knob { background: #4f86c6; }

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
  .dpad-btn:active { background: #3a6ea5; border-color: #3a6ea5; }
  .dpad-up    { grid-area: 1 / 2 / 2 / 3; }
  .dpad-left  { grid-area: 2 / 1 / 3 / 2; }
  .dpad-stop  { grid-area: 2 / 2 / 3 / 3; }
  .dpad-right { grid-area: 2 / 3 / 3 / 4; }
  .dpad-down  { grid-area: 3 / 2 / 4 / 3; }

  .modal-overlay {
    display: none;
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.6);
    align-items: center;
    justify-content: center;
    z-index: 10;
    padding: 16px;
  }
  .modal-overlay.show { display: flex; }
  .modal-panel {
    background: #1f2a33;
    border: 1px solid #334351;
    border-radius: 10px;
    padding: 18px;
    width: 100%;
    max-width: 300px;
  }
  .modal-title {
    font-size: 0.95rem;
    font-weight: bold;
    margin-bottom: 14px;
  }
  .option-group {
    margin-bottom: 16px;
  }
  .option-label {
    font-size: 0.75rem;
    color: #7a8a97;
    letter-spacing: 1px;
    margin-bottom: 6px;
  }
  .option-radio, .option-checkbox {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 0.85rem;
    padding: 6px 0;
    cursor: pointer;
  }
  .option-radio input, .option-checkbox input {
    width: 16px;
    height: 16px;
  }
  .modal-close {
    width: 100%;
    margin-top: 4px;
  }

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
    <button class="settings-btn" id="settingsBtn" title="Opciones">⚙</button>
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

  <div class="joy-wrap" id="joystickPanel">
    <div class="joy-title">MOVIMIENTO (joystick)</div>
    <div class="joystick" id="joystick">
      <div class="joystick-knob" id="joystickKnob"></div>
    </div>
    <div class="joy-hint">Arrastrar el joystick, o usar flechas/WASD en el teclado. Adelante + un costado traza una curva abierta; un costado solo (sin avanzar) gira en el lugar.</div>
  </div>

  <div class="joy-wrap" id="dpadPanel" style="display:none">
    <div class="joy-title">MOVIMIENTO (D-pad clásico)</div>
    <div class="dpad">
      <button class="dpad-btn dpad-up"    data-dir="forward">▲</button>
      <button class="dpad-btn dpad-left"  data-dir="left">◀</button>
      <button class="dpad-btn dpad-stop"  data-dir="stop">■</button>
      <button class="dpad-btn dpad-right" data-dir="right">▶</button>
      <button class="dpad-btn dpad-down"  data-dir="backward">▼</button>
    </div>
    <div class="joy-hint">Girar en el lugar (izquierda/derecha pivotan), igual que antes. También funciona con flechas/WASD.</div>
  </div>

  <div class="modal-overlay" id="optionsOverlay">
    <div class="modal-panel">
      <div class="modal-title">Opciones</div>

      <div class="option-group">
        <div class="option-label">MODO DE CONTROL</div>
        <label class="option-radio">
          <input type="radio" name="controlMode" value="joystick">
          Joystick (curvas abiertas)
        </label>
        <label class="option-radio">
          <input type="radio" name="controlMode" value="dpad">
          D-pad clásico (pivotar)
        </label>
      </div>

      <div class="option-group">
        <label class="option-checkbox">
          <input type="checkbox" id="invertControlsChk">
          Invertir controles (adelante/atrás e izquierda/derecha)
        </label>
      </div>

      <button class="modal-close" id="optionsCloseBtn">Cerrar</button>
    </div>
  </div>

  <div class="diag-wrap">
    <div class="joy-title">DIAGNÓSTICO - SALIDAS INDIVIDUALES (PCF8574)</div>
    <div class="diag-row">
      <button class="diag-btn" data-pin="0">IN1</button>
      <button class="diag-btn" data-pin="1">IN2</button>
      <button class="diag-btn" data-pin="2">IN3</button>
      <button class="diag-btn" data-pin="4">IN4</button>
    </div>
  </div>

<script>
  const statusEl = document.getElementById('status');

  // Opciones persistentes (por navegador, no por el firmware): modo de
  // control (joystick con curvas abiertas, o D-pad clásico que pivota) e
  // inversión de controles. Se guardan en localStorage para no tener que
  // volver a elegirlas cada vez que se abre la página.
  const OPTIONS_KEY = 'rover-tito-options';
  let controlMode = 'joystick'; // 'joystick' | 'dpad'
  let invertControls = false;

  function loadOptions() {
    try {
      const raw = localStorage.getItem(OPTIONS_KEY);
      if (!raw) return;
      const opts = JSON.parse(raw);
      if (opts.controlMode === 'joystick' || opts.controlMode === 'dpad') {
        controlMode = opts.controlMode;
      }
      invertControls = !!opts.invertControls;
    } catch (e) { /* localStorage no disponible (ventana privada, etc.) */ }
  }

  function saveOptions() {
    try {
      localStorage.setItem(OPTIONS_KEY, JSON.stringify({ controlMode, invertControls }));
    } catch (e) { /* ignorar: la opción sigue funcionando, sólo no persiste */ }
  }

  // Control de movimiento tipo "juego de carreras": throttle (adelante/atrás)
  // + steer (izquierda/derecha), mezclados con la fórmula clásica de manejo
  // arcade (l = throttle + steer, r = throttle - steer) para obtener la
  // velocidad de cada rueda por separado. Avanzar + doblar da una curva
  // abierta (la rueda interna sólo frena, no se invierte); doblar sin avanzar
  // sigue girando en el lugar, como antes. Alimentan esta misma lógica tanto
  // el joystick táctil, el teclado, como el D-pad clásico (ver más abajo).
  //
  // El resultado (l, r) se manda a /drive?l=..&r=.. y se reenvía cada 150ms
  // mientras haya movimiento (keepalive) para sostener el watchdog de
  // seguridad del firmware (frena solo si deja de recibir comandos).
  let currentL = 0;
  let currentR = 0;
  let keepAliveTimer = null;

  function sendDrive(l, r) {
    fetch('/drive?l=' + l + '&r=' + r)
      .then(() => { statusEl.textContent = 'conectado'; })
      .catch(() => { statusEl.textContent = 'sin conexión'; });
  }

  function setDrive(throttle, steer) {
    if (invertControls) {
      throttle = -throttle;
      steer = -steer;
    }
    let l = Math.max(-1, Math.min(1, throttle + steer));
    let r = Math.max(-1, Math.min(1, throttle - steer));
    currentL = Math.round(l * 100);
    currentR = Math.round(r * 100);
    sendDrive(currentL, currentR);

    if (currentL === 0 && currentR === 0) {
      clearInterval(keepAliveTimer);
      keepAliveTimer = null;
    } else if (!keepAliveTimer) {
      keepAliveTimer = setInterval(() => sendDrive(currentL, currentR), 150);
    }
  }

  // Joystick táctil/mouse: el centro es throttle=0/steer=0, y se satura en
  // el borde del círculo (radio JOY_RADIUS) a throttle=±1/steer=±1.
  const joystick = document.getElementById('joystick');
  const joystickKnob = document.getElementById('joystickKnob');
  const JOY_RADIUS = 95; // mitad del ancho/alto de .joystick (190px)
  let joyActive = false;

  function setKnobPos(dx, dy) {
    joystickKnob.style.transform = 'translate(calc(-50% + ' + dx + 'px), calc(-50% + ' + dy + 'px))';
  }

  function updateFromPointer(clientX, clientY) {
    const rect = joystick.getBoundingClientRect();
    let dx = clientX - (rect.left + rect.width / 2);
    let dy = clientY - (rect.top + rect.height / 2);
    const dist = Math.hypot(dx, dy);
    if (dist > JOY_RADIUS) {
      dx = dx / dist * JOY_RADIUS;
      dy = dy / dist * JOY_RADIUS;
    }
    setKnobPos(dx, dy);
    setDrive(-dy / JOY_RADIUS, dx / JOY_RADIUS);
  }

  function endJoystick() {
    if (!joyActive) return;
    joyActive = false;
    joystick.classList.remove('active');
    setKnobPos(0, 0);
    setDrive(0, 0);
  }

  joystick.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    joyActive = true;
    joystick.classList.add('active');
    joystick.setPointerCapture(e.pointerId);
    updateFromPointer(e.clientX, e.clientY);
  });
  joystick.addEventListener('pointermove', (e) => {
    if (joyActive) updateFromPointer(e.clientX, e.clientY);
  });
  joystick.addEventListener('pointerup', endJoystick);
  joystick.addEventListener('pointercancel', endJoystick);

  // D-pad clásico: un solo botón activo a la vez (como antes de agregar el
  // joystick), con las mismas combinaciones throttle/steer de setDrive() -
  // adelante=(1,0), atrás=(-1,0), izquierda/derecha pivotan en el lugar
  // =(0,∓1). "last key/botón gana", sin combinar direcciones.
  const DPAD_VECTOR = {
    forward:  [1, 0],
    backward: [-1, 0],
    left:     [0, -1],
    right:    [0, 1],
  };
  let legacyDir = 'stop';

  function startMoveLegacy(dir) {
    if (dir === 'stop' || dir === legacyDir) return;
    legacyDir = dir;
    const [t, s] = DPAD_VECTOR[dir];
    setDrive(t, s);
  }

  function stopMoveLegacy() {
    if (legacyDir === 'stop') return;
    legacyDir = 'stop';
    setDrive(0, 0);
  }

  document.querySelectorAll('.dpad-btn').forEach((btn) => {
    const dir = btn.dataset.dir;
    if (dir === 'stop') {
      btn.addEventListener('click', stopMoveLegacy);
      return;
    }
    btn.addEventListener('pointerdown', (e) => { e.preventDefault(); startMoveLegacy(dir); });
    btn.addEventListener('pointerup', stopMoveLegacy);
    btn.addEventListener('pointerleave', stopMoveLegacy);
    btn.addEventListener('pointercancel', stopMoveLegacy);
  });

  // Teclado (flechas / WASD). En modo joystick, adelante/atrás + izquierda/
  // derecha se combinan con la mezcla arcade (presionar adelante y un
  // costado da una curva abierta). En modo D-pad clásico, se comporta como
  // los botones: una sola dirección activa a la vez (gana la última tecla
  // presionada), para pivotar igual que el D-pad táctil.
  const keysDown = new Set();
  const KEY_DIR = {
    ArrowUp: 'forward', w: 'forward', W: 'forward',
    ArrowDown: 'backward', s: 'backward', S: 'backward',
    ArrowLeft: 'left', a: 'left', A: 'left',
    ArrowRight: 'right', d: 'right', D: 'right'
  };

  function updateFromKeyboard() {
    let throttle = 0;
    let steer = 0;
    if (keysDown.has('forward')) throttle += 1;
    if (keysDown.has('backward')) throttle -= 1;
    if (keysDown.has('left')) steer -= 1;
    if (keysDown.has('right')) steer += 1;
    setDrive(throttle, steer);
  }

  window.addEventListener('keydown', (e) => {
    const dir = KEY_DIR[e.key];
    if (!dir) return;
    if (controlMode === 'dpad') {
      startMoveLegacy(dir);
      return;
    }
    if (keysDown.has(dir)) return;
    keysDown.add(dir);
    updateFromKeyboard();
  });
  window.addEventListener('keyup', (e) => {
    const dir = KEY_DIR[e.key];
    if (!dir) return;
    if (controlMode === 'dpad') {
      stopMoveLegacy();
      return;
    }
    keysDown.delete(dir);
    updateFromKeyboard();
  });

  // Panel de opciones: modo de control (joystick / D-pad clásico) e
  // inversión de controles. Al cambiar de modo se frena todo y se limpia el
  // estado de ambos esquemas, para no dejar una dirección "pegada".
  const joystickPanel = document.getElementById('joystickPanel');
  const dpadPanel = document.getElementById('dpadPanel');
  const settingsBtn = document.getElementById('settingsBtn');
  const optionsOverlay = document.getElementById('optionsOverlay');
  const optionsCloseBtn = document.getElementById('optionsCloseBtn');
  const invertControlsChk = document.getElementById('invertControlsChk');
  const controlModeRadios = document.querySelectorAll('input[name="controlMode"]');

  function stopAllMovement() {
    keysDown.clear();
    legacyDir = 'stop';
    joyActive = false;
    joystick.classList.remove('active');
    setKnobPos(0, 0);
    setDrive(0, 0);
  }

  function applyControlMode() {
    joystickPanel.style.display = controlMode === 'joystick' ? '' : 'none';
    dpadPanel.style.display = controlMode === 'dpad' ? '' : 'none';
    stopAllMovement();
  }

  function openOptions() {
    stopAllMovement(); // frena mientras se configura, por seguridad
    controlModeRadios.forEach((r) => { r.checked = (r.value === controlMode); });
    invertControlsChk.checked = invertControls;
    optionsOverlay.classList.add('show');
  }

  function closeOptions() {
    optionsOverlay.classList.remove('show');
  }

  settingsBtn.addEventListener('click', openOptions);
  optionsCloseBtn.addEventListener('click', closeOptions);
  optionsOverlay.addEventListener('click', (e) => {
    if (e.target === optionsOverlay) closeOptions();
  });

  controlModeRadios.forEach((radio) => {
    radio.addEventListener('change', () => {
      if (!radio.checked) return;
      controlMode = radio.value;
      saveOptions();
      applyControlMode();
    });
  });

  invertControlsChk.addEventListener('change', () => {
    invertControls = invertControlsChk.checked;
    saveOptions();
  });

  loadOptions();
  applyControlMode();

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
  sendDrive(0, 0);

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
// Control de motores (puente H L298N vía PCF8574) - tracción diferencial con
// velocidad progresiva por rueda (PWM por software, ver comentario de arriba
// del archivo sobre por qué no hay PWM de hardware).
// ---------------------------------------------------------------------------
// Velocidad pedida para cada rueda, -100 (atrás a fondo) .. 100 (adelante a
// fondo), 0 = detenida. La fija handleDrive() desde la web (joystick o
// teclado) y la resetea a 0 el watchdog de seguridad.
int wheelSpeedLeft = 0;
int wheelSpeedRight = 0;

// Período del PWM por software y duty mínimo. Por debajo de PWM_MIN_DUTY_PCT
// el motor puede no tener torque suficiente para arrancar (fricción/carga),
// así que cualquier velocidad distinta de cero se "redondea para arriba" a
// ese mínimo - evita pedir una curva abierta y que la rueda interna no gire.
// Ajustar ambas constantes si el motor/batería usados dan otro resultado.
#define PWM_PERIOD_MS    20   // 50Hz
#define PWM_MIN_DUTY_PCT 30

// Devuelve si el bit de una rueda debe estar encendido en este instante del
// ciclo PWM, según el duty cycle pedido (0-100, ya en valor absoluto).
static bool pwmBitOn(int dutyPct, unsigned long nowMs) {
  if (dutyPct <= 0) return false;
  if (dutyPct < PWM_MIN_DUTY_PCT) dutyPct = PWM_MIN_DUTY_PCT;
  if (dutyPct > 100) dutyPct = 100;
  unsigned long phase = nowMs % PWM_PERIOD_MS;
  unsigned long threshold = (unsigned long)dutyPct * PWM_PERIOD_MS / 100;
  return phase < threshold;
}

// Recalcula las 4 salidas del PCF8574 a partir de wheelSpeedLeft/Right y la
// fase actual del PWM, y las escribe por I2C sólo si cambiaron (para no
// saturar el bus con escrituras idénticas). Hay que llamarla seguido desde
// loop() para que el PWM tenga buena resolución. No hace nada mientras hay
// un test de diagnóstico activo (ver diagActive).
static void updateMotorPWM() {
  if (diagActive) return;

  uint8_t out = 0x00;
  unsigned long now = millis();

  if (wheelSpeedLeft != 0 && pwmBitOn(abs(wheelSpeedLeft), now)) {
    out |= (1 << (wheelSpeedLeft > 0 ? MOTOR_IZQ_IN1 : MOTOR_IZQ_IN2));
  }
  if (wheelSpeedRight != 0 && pwmBitOn(abs(wheelSpeedRight), now)) {
    out |= (1 << (wheelSpeedRight > 0 ? MOTOR_DER_IN1 : MOTOR_DER_IN2));
  }

  if (out != pcf8574State) pcf8574Write(out);
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

// /drive?l=-100..100&r=-100..100 -> velocidad (duty cycle con signo) de cada
// rueda, calculada en el navegador a partir del joystick o del teclado.
void handleDrive() {
  int l = server.hasArg("l") ? server.arg("l").toInt() : 0;
  int r = server.hasArg("r") ? server.arg("r").toInt() : 0;
  l = constrain(l, -100, 100);
  r = constrain(r, -100, 100);

  diagActive = false; // un comando de /drive corta cualquier test de diagnóstico en curso
  wheelSpeedLeft = l;
  wheelSpeedRight = r;
  motorsRunning = (l != 0 || r != 0);
  lastMoveCmdMs = millis();

  server.send(200, "text/plain", "ok");
}

// /testPin?pin=0-7&state=on|off -> diagnóstico: activa una sola salida del
// PCF8574 (P0, P1, P2, P4, las usadas por IN1-IN4) sin pasar por el PWM por
// software de las ruedas. Usa el mismo watchdog que /drive.
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

  diagActive = on;
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
  wheelSpeedLeft = 0;
  wheelSpeedRight = 0;
  updateMotorPWM(); // motores detenidos al arrancar

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);

  Serial.println();
  Serial.println("Red WiFi creada:");
  Serial.printf("  SSID:     %s\n", AP_SSID);
  Serial.printf("  Password: %s\n", AP_PASSWORD);
  Serial.printf("  IP:       http://%s/\n", WiFi.softAPIP().toString().c_str());

  server.on("/", handleIndex);
  server.on("/drive", handleDrive);
  server.on("/testPin", handleTestPin);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("Servidor web listo.");
}

void loop() {
  server.handleClient();
  updateMotorPWM();

  // Watchdog de movimiento: si dejan de llegar comandos de /drive (por un
  // corte de WiFi, por ejemplo) se frenan los motores solos.
  if (motorsRunning && (millis() - lastMoveCmdMs > MOVE_TIMEOUT_MS)) {
    wheelSpeedLeft = 0;
    wheelSpeedRight = 0;
    if (diagActive) {
      diagActive = false;
      pcf8574Write(0x00);
    }
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
