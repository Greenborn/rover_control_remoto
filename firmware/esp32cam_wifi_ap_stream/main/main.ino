/*
 * ESP32-CAM - Access Point + visor de cámara en vivo (MJPEG)
 *
 * Al encender, el módulo crea su propia red WiFi (modo Access Point).
 * Cualquier dispositivo que se conecte a esa red puede abrir
 * http://192.168.4.1/ en el navegador y ver el video en directo.
 *
 * Placa: AI-Thinker ESP32-CAM (OV2640)
 * Board manager: "AI Thinker ESP32-CAM" (paquete esp32 de Espressif)
 *
 * Librerías usadas: todas incluidas en el core esp32 de Arduino
 * (WiFi.h, esp_camera.h, esp_http_server.h) - no requiere instalar nada extra.
 */

#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// ---------------------------------------------------------------------------
// Configuración de la red WiFi que genera el ESP32-CAM
// ---------------------------------------------------------------------------
const char *AP_SSID     = "ROVER-CAM";     // Nombre de la red WiFi
const char *AP_PASSWORD = "rover1234";     // Mínimo 8 caracteres. Poner "" para red abierta.
const int   AP_CHANNEL  = 1;               // Canal WiFi (1-13)
const int   AP_MAX_CONN = 4;               // Cantidad máxima de clientes conectados

// IP fija del AP (por defecto el ESP32 usa 192.168.4.1, se define explícito)
IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ---------------------------------------------------------------------------
// Pines de cámara - módulo AI-Thinker ESP32-CAM
// ---------------------------------------------------------------------------
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// LED flash integrado (GPIO4) - opcional, se puede controlar desde la web
#define FLASH_LED_GPIO     4

// ---------------------------------------------------------------------------
// Pines de motores - puente H L298N (tracción diferencial)
// ENA/ENB del L298N están puenteados a 5V (siempre habilitados), por eso
// alcanza con manejar IN1/IN2 de cada motor a velocidad fija, sin PWM.
// ---------------------------------------------------------------------------
#define MOTOR_IZQ_IN1     12
#define MOTOR_IZQ_IN2     13
#define MOTOR_DER_IN1     14
#define MOTOR_DER_IN2     15

enum MotorDir { MOTOR_STOP, MOTOR_FORWARD, MOTOR_BACKWARD };

// Watchdog de movimiento: si no llega un comando de /move en este tiempo
// (por ejemplo por un corte de WiFi) se frenan los motores solos.
#define MOVE_TIMEOUT_MS   800
volatile unsigned long lastMoveCmdMs = 0;
volatile bool motorsRunning = false;

httpd_handle_t pageServer   = NULL;
httpd_handle_t streamServer = NULL;

// ---------------------------------------------------------------------------
// Página HTML principal (visor de la cámara)
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ROVER CAM</title>
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
  .video-wrap {
    width: 100%;
    max-width: 720px;
    margin: 16px auto;
    padding: 0 12px;
  }
  #stream {
    width: 100%;
    border-radius: 10px;
    border: 1px solid #1f2a33;
    background: #000;
    display: block;
  }
  .controls {
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    justify-content: center;
    margin: 10px 0 30px;
    padding: 0 12px;
  }
  button, select {
    background: #1f2a33;
    color: #e8edf2;
    border: 1px solid #334351;
    border-radius: 8px;
    padding: 10px 16px;
    font-size: 0.9rem;
    cursor: pointer;
  }
  button:active { background: #334351; }
  button.active { background: #3a6ea5; border-color: #3a6ea5; }

  .dpad-wrap {
    margin: 0 0 30px;
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
    grid-template-columns: 56px 56px 56px;
    grid-template-rows: 56px 56px 56px;
    gap: 6px;
  }
  .dpad-btn {
    font-size: 1.3rem;
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
    <h1>ROVER CAM</h1>
    <div class="status" id="status">conectando...</div>
  </header>

  <div class="video-wrap">
    <img id="stream">
  </div>

  <div class="controls">
    <button id="flashBtn" onclick="toggleFlash()">💡 Flash</button>
    <select id="resSelect" onchange="setResolution(this.value)">
      <option value="10">Resolución: Alta (UXGA)</option>
      <option value="8">Resolución: Media (SVGA)</option>
      <option value="6" selected>Resolución: Normal (VGA)</option>
      <option value="4">Resolución: Baja (QVGA)</option>
    </select>
  </div>

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
  const img = document.getElementById('stream');
  const statusEl = document.getElementById('status');
  let flashOn = false;

  // El video MJPEG se sirve en un servidor HTTP aparte, en el puerto 81
  // (ver startCameraServer() en el firmware). Así el servidor del puerto 80
  // (página, flash, resolución) queda libre para responder mientras el
  // stream está activo: cada servidor de esp_http_server corre en su propio
  // hilo, y el handler de /stream nunca retorna (loop infinito), así que si
  // compartiera servidor con el resto bloquearía todos los demás pedidos.
  const streamBase = 'http://' + window.location.hostname + ':81/stream';

  img.onload = () => { statusEl.textContent = 'en vivo'; };
  img.onerror = () => {
    statusEl.textContent = 'reconectando...';
    setTimeout(() => { img.src = streamBase + '?' + Date.now(); }, 1000);
  };

  img.src = streamBase;

  function toggleFlash() {
    flashOn = !flashOn;
    fetch('/flash?state=' + (flashOn ? '1' : '0'));
    document.getElementById('flashBtn').classList.toggle('active', flashOn);
  }

  function setResolution(val) {
    fetch('/control?var=framesize&val=' + val);
  }

  // D-pad de movimiento: al presionar se manda la dirección y se reenvía
  // cada 400ms mientras se mantiene (keepalive) para sostener el watchdog
  // de seguridad del firmware (frena solo si deja de recibir comandos).
  let currentDir = 'stop';
  let keepAliveTimer = null;

  function sendMove(dir) {
    fetch('/move?dir=' + dir);
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
</script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------
// Handler: página principal
// ---------------------------------------------------------------------------
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

// ---------------------------------------------------------------------------
// Handler: /flash?state=0|1
// ---------------------------------------------------------------------------
static esp_err_t flash_handler(httpd_req_t *req) {
  char query[32];
  bool turnOn = false;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[4];
    if (httpd_query_key_value(query, "state", val, sizeof(val)) == ESP_OK) {
      turnOn = (val[0] == '1');
    }
  }
  digitalWrite(FLASH_LED_GPIO, turnOn ? HIGH : LOW);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "ok", 2);
}

// ---------------------------------------------------------------------------
// Handler: /control?var=framesize&val=N  (ajustes de cámara en caliente)
// ---------------------------------------------------------------------------
static esp_err_t control_handler(httpd_req_t *req) {
  char query[64];
  char varName[32] = {0};
  char varVal[16]  = {0};

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "var", varName, sizeof(varName));
    httpd_query_key_value(query, "val", varVal, sizeof(varVal));
  }

  sensor_t *s = esp_camera_sensor_get();
  int val = atoi(varVal);

  if (s != NULL && strcmp(varName, "framesize") == 0) {
    s->set_framesize(s, (framesize_t)val);
  }

  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "ok", 2);
}

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
// Handler: /move?dir=forward|backward|left|right|stop
// ---------------------------------------------------------------------------
static esp_err_t move_handler(httpd_req_t *req) {
  char query[32];
  char dir[16] = "stop";
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "dir", dir, sizeof(dir));
  }

  driveRover(dir);
  motorsRunning = (strcmp(dir, "stop") != 0);
  lastMoveCmdMs = millis();

  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "ok", 2);
}

// ---------------------------------------------------------------------------
// Handler: /stream  (MJPEG - multipart/x-mixed-replace)
// ---------------------------------------------------------------------------
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        // El sensor está configurado en JPEG, esto no debería pasar.
        esp_camera_fb_return(fb);
        res = ESP_FAIL;
      } else {
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
          size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
          res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
          res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        esp_camera_fb_return(fb);
      }
    }
    if (res != ESP_OK) {
      break;
    }
  }
  return res;
}

// ---------------------------------------------------------------------------
// Inicio de los servidores HTTP
// ---------------------------------------------------------------------------
void startCameraServer() {
  // Dos servidores HTTP separados, cada uno en su propio hilo (así es como
  // trabaja esp_http_server: un solo worker por instancia, que procesa sus
  // pedidos de a uno). El handler de "/stream" no retorna nunca (loop
  // infinito mientras el cliente esté conectado), así que si compartiera
  // servidor con "/", "/flash" y "/control" los bloquearía por completo
  // mientras el video está activo. Separarlos en el puerto 80 (página y
  // controles) y 81 (stream) permite usarlos al mismo tiempo. Es el mismo
  // esquema que usa el ejemplo oficial CameraWebServer de Espressif.
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };
  httpd_uri_t flash_uri = {
    .uri = "/flash",
    .method = HTTP_GET,
    .handler = flash_handler,
    .user_ctx = NULL
  };
  httpd_uri_t control_uri = {
    .uri = "/control",
    .method = HTTP_GET,
    .handler = control_handler,
    .user_ctx = NULL
  };
  httpd_uri_t move_uri = {
    .uri = "/move",
    .method = HTTP_GET,
    .handler = move_handler,
    .user_ctx = NULL
  };

  if (httpd_start(&pageServer, &config) == ESP_OK) {
    httpd_register_uri_handler(pageServer, &index_uri);
    httpd_register_uri_handler(pageServer, &flash_uri);
    httpd_register_uri_handler(pageServer, &control_uri);
    httpd_register_uri_handler(pageServer, &move_uri);
    Serial.println("Servidor de pagina (puerto 80): OK");
  } else {
    Serial.println("Servidor de pagina (puerto 80): FALLO al iniciar");
  }

  config.server_port = 81;
  config.ctrl_port = 32769; // debe ser distinto al del primer servidor

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  if (httpd_start(&streamServer, &config) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &stream_uri);
    Serial.println("Servidor de stream (puerto 81): OK");
  } else {
    Serial.println("Servidor de stream (puerto 81): FALLO al iniciar");
  }
}

// ---------------------------------------------------------------------------
// Inicialización de la cámara
// ---------------------------------------------------------------------------
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // Si hay PSRAM disponible (lo normal en el AI-Thinker) usamos mejor calidad.
  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  Serial.printf("PSRAM detectada: %s\n", psramFound() ? "si" : "no");

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error iniciando la camara: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("Sensor detectado, PID: 0x%x\n", s->id.PID);
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }

  return true;
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);

  pinMode(FLASH_LED_GPIO, OUTPUT);
  digitalWrite(FLASH_LED_GPIO, LOW);

  pinMode(MOTOR_IZQ_IN1, OUTPUT);
  pinMode(MOTOR_IZQ_IN2, OUTPUT);
  pinMode(MOTOR_DER_IN1, OUTPUT);
  pinMode(MOTOR_DER_IN2, OUTPUT);
  driveRover("stop"); // motores detenidos al arrancar

  if (!initCamera()) {
    Serial.println("No se pudo inicializar la camara. Revisar conexiones/placa seleccionada.");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);

  Serial.println();
  Serial.println("Red WiFi creada:");
  Serial.printf("  SSID:     %s\n", AP_SSID);
  Serial.printf("  Password: %s\n", AP_PASSWORD);
  Serial.printf("  IP:       http://%s/\n", WiFi.softAPIP().toString().c_str());

  startCameraServer();
  Serial.println("Servidor de camara listo.");
}

void loop() {
  // Todo el trabajo lo hacen los handlers HTTP (tareas internas del esp_http_server).
  // Watchdog de movimiento: si dejan de llegar comandos de /move (por un
  // corte de WiFi, por ejemplo) se frenan los motores solos.
  if (motorsRunning && (millis() - lastMoveCmdMs > MOVE_TIMEOUT_MS)) {
    driveRover("stop");
    motorsRunning = false;
    Serial.println("Motores detenidos por timeout de seguridad (sin señal).");
  }
  delay(100);
}
