/*
 * ESP8266 - Access Point + control de motores (puente H L298N)
 *
 * Al encender, el módulo crea su propia red WiFi (modo Access Point).
 * Cualquier dispositivo que se conecte a esa red puede abrir
 * http://192.168.4.1/ en el navegador y manejar el rover con un D-pad.
 *
 * Sin cámara: este sketch es la versión "solo motores" para un ESP8266
 * separado del ESP32-CAM (ver ../esp32cam_wifi_ap_stream).
 *
 * Placa: NodeMCU / Wemos D1 Mini (ESP8266, módulo ESP-12E).
 * Board manager: "NodeMCU 1.0 (ESP-12E Module)" o "LOLIN(WEMOS) D1 R2 & mini"
 * (paquete esp8266 de la comunidad Arduino) - ambas usan el mismo mapeo D1..D8.
 *
 * Librerías usadas: todas incluidas en el core esp8266 de Arduino
 * (ESP8266WiFi.h, ESP8266WebServer.h) - no requiere instalar nada extra.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// ---------------------------------------------------------------------------
// Configuración de la red WiFi que genera el ESP8266
// ---------------------------------------------------------------------------
const char *AP_SSID     = "ROVER-MOTOR";   // Nombre de la red WiFi
const char *AP_PASSWORD = "rover1234";     // Mínimo 8 caracteres. Poner "" para red abierta.
const int   AP_CHANNEL  = 1;               // Canal WiFi (1-13)
const int   AP_MAX_CONN = 4;               // Cantidad máxima de clientes conectados

// IP fija del AP (por defecto el ESP8266 usa 192.168.4.1, se define explícito)
IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ---------------------------------------------------------------------------
// Pines de motores - puente H L298N (tracción diferencial)
// ENA/ENB del L298N están puenteados a 5V (siempre habilitados), por eso
// alcanza con manejar IN1/IN2 de cada motor a velocidad fija, sin PWM.
// D1/D2/D6/D7 son los nombres estándar del core esp8266 (NodeMCU/Wemos D1
// Mini): D1=GPIO5, D2=GPIO4, D6=GPIO12, D7=GPIO13.
// ---------------------------------------------------------------------------
#define MOTOR_IZQ_IN1   D1
#define MOTOR_IZQ_IN2   D2
#define MOTOR_DER_IN1   D6
#define MOTOR_DER_IN2   D7

enum MotorDir { MOTOR_STOP, MOTOR_FORWARD, MOTOR_BACKWARD };

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
<title>ROVER MOTOR</title>
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: #10151a;
    color: #e8edf2;
    font-family: -apple-system, Segoe UI, Roboto, Arial, sans-serif;
    display: flex;
    flex-direction: column;
    align-items: center;
    min-height: 100vh;
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
  button {
    background: #1f2a33;
    color: #e8edf2;
    border: 1px solid #334351;
    border-radius: 8px;
    padding: 10px 16px;
    font-size: 0.9rem;
    cursor: pointer;
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
</style>
</head>
<body>
  <header>
    <h1>ROVER MOTOR</h1>
    <div class="status" id="status">conectando...</div>
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

  // Confirma la conexión al cargar la página (y deja los motores detenidos).
  sendMove('stop');
</script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------
// Control de motores (puente H L298N) - tracción diferencial
// ---------------------------------------------------------------------------
static void setMotor(int in1, int in2, MotorDir dir) {
  digitalWrite(in1, dir == MOTOR_FORWARD  ? HIGH : LOW);
  digitalWrite(in2, dir == MOTOR_BACKWARD ? HIGH : LOW);
}

// dir: "forward" | "backward" | "left" | "right" | "stop"
// Para girar en el lugar, una rueda va adelante y la otra atrás.
static void driveRover(const char *dir) {
  if (strcmp(dir, "forward") == 0) {
    setMotor(MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_FORWARD);
    setMotor(MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_FORWARD);
  } else if (strcmp(dir, "backward") == 0) {
    setMotor(MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_BACKWARD);
    setMotor(MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_BACKWARD);
  } else if (strcmp(dir, "left") == 0) {
    setMotor(MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_BACKWARD);
    setMotor(MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_FORWARD);
  } else if (strcmp(dir, "right") == 0) {
    setMotor(MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_FORWARD);
    setMotor(MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_BACKWARD);
  } else {
    setMotor(MOTOR_IZQ_IN1, MOTOR_IZQ_IN2, MOTOR_STOP);
    setMotor(MOTOR_DER_IN1, MOTOR_DER_IN2, MOTOR_STOP);
  }
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

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(MOTOR_IZQ_IN1, OUTPUT);
  pinMode(MOTOR_IZQ_IN2, OUTPUT);
  pinMode(MOTOR_DER_IN1, OUTPUT);
  pinMode(MOTOR_DER_IN2, OUTPUT);
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
}
