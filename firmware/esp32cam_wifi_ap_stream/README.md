# ESP32-CAM — Access Point con visor de cámara en vivo

Este sketch hace que el módulo **ESP32-CAM (AI-Thinker)**, al encenderse, cree su propia
red WiFi (modo *Access Point*) y sirva una página web con el video de la cámara en
directo (MJPEG). No necesita router ni conexión a internet: te conectás directamente
al ESP32-CAM desde el celular o la PC.

## Qué hace

1. Al bootear, crea la red WiFi `ROVER-CAM` (contraseña `rover1234` por defecto).
2. Levanta un servidor web en `http://192.168.4.1/`.
3. La página muestra el video en vivo, un botón de flash (LED integrado) y un selector
   de resolución.
4. El streaming va por `http://192.168.4.1:81/stream` (puerto separado para no trabar
   la página principal).

## Estructura

```
esp32cam_wifi_ap_stream/
├── main/
│   └── main.ino        # sketch (todo el firmware en un solo archivo)
├── build.sh             # compila con arduino-cli
├── flash.sh             # flashea por USB con esptool
├── monitor.sh           # abre el monitor serie
├── install_venv.sh      # crea el venv local con esptool/pyserial
└── README.md
```

## Requisitos

- Placa **ESP32-CAM** (módulo AI-Thinker, con sensor OV2640).
- Programador FTDI / USB-TTL para flashear (el ESP32-CAM no tiene USB propio).
- No se necesitan librerías externas: `WiFi.h`, `esp_camera.h` y `esp_http_server.h`
  vienen incluidas en el core de esp32.
- Para compilar/flashear por línea de comandos (scripts de abajo): `arduino-cli`
  con el core `esp32:esp32` instalado, y Python 3 (para el venv de `esptool`).
- Alternativa: Arduino IDE con el paquete de placas **esp32 by Espressif Systems**
  instalado (Boards Manager → buscar "esp32"), board **"AI Thinker ESP32-CAM"**,
  Partition Scheme **"Huge APP (3MB No OTA/1MB SPIFFS)"**.

## Compilar, flashear y monitorear (línea de comandos)

```bash
./build.sh              # compila el sketch con arduino-cli -> build/
./flash.sh               # auto-detecta el puerto y flashea build/main.ino.merged.bin
./monitor.sh             # abre el monitor serie a 115200 baud (Ctrl-] para salir)
```

`flash.sh` y `monitor.sh` instalan automáticamente un venv local (`.venv/`) con
`esptool`/`pyserial` la primera vez que se usan (también se puede forzar con
`./install_venv.sh`). Opciones útiles:

```bash
./build.sh --serial            # compila en un solo hilo (por defecto usa todos los núcleos)
./flash.sh --port /dev/ttyUSB0 # puerto explícito en vez de auto-detección
./flash.sh --erase             # borra toda la flash antes de flashear
./monitor.sh --baud 115200     # velocidad serie explícita
```

## Cableado para flashear

El ESP32-CAM no tiene conversor USB integrado, hay que usar un adaptador FTDI/USB-TTL:

| FTDI | ESP32-CAM |
|------|-----------|
| 5V   | 5V        |
| GND  | GND       |
| TX   | U0R (RX)  |
| RX   | U0T (TX)  |

Para entrar en modo programación, unir **GPIO0 con GND** antes de resetear/alimentar,
subir el sketch, y luego desconectar ese puente y resetear para que arranque normal.

## Personalización

Al principio del archivo `.ino` están las constantes para ajustar:

```cpp
const char *AP_SSID     = "ROVER-CAM";
const char *AP_PASSWORD = "rover1234";   // mínimo 8 caracteres, o "" para red abierta
const int   AP_CHANNEL  = 1;
const int   AP_MAX_CONN = 4;
```

## Uso

1. Flashear el sketch.
2. Alimentar el ESP32-CAM (5V).
3. Desde el celular/PC, conectarse a la red WiFi `ROVER-CAM`.
4. Abrir el navegador en `http://192.168.4.1/`.
5. Se ve el video en vivo; con los controles de la página se puede prender el flash
   o cambiar la resolución en caliente.

## Notas

- Si el LED de flash no responde, verificar que el GPIO4 no esté en uso por otra
  función en tu variante de placa.
- Si la imagen tarda en cargar o se ve muy pixelada, bajar la resolución desde el
  selector de la página (menos ancho de banda necesario).
- Este modo AP es ideal para controlar la cámara del rover sin depender de una red
  WiFi externa; si en el futuro se necesita que el ESP32-CAM se conecte a una red
  existente (modo estación) en lugar de crear la suya, hay que cambiar `WiFi.mode(WIFI_AP)`
  por `WIFI_STA` y usar `WiFi.begin(ssid, password)`.
