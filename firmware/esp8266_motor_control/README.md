# ESP8266 — Access Point con control de motores (sin cámara)

Este sketch hace que un módulo **ESP8266** (NodeMCU / Wemos D1 Mini), al encenderse,
cree su propia red WiFi (modo *Access Point*) y sirva una página web con un D-pad
para manejar un rover de tracción diferencial a través de un puente H **L298N**.
No necesita router ni conexión a internet: te conectás directamente al ESP8266
desde el celular o la PC. No tiene cámara (para eso ver `../esp32cam_wifi_ap_stream`).

## Qué hace

1. Al bootear, crea la red WiFi `ROVER-MOTOR` (contraseña `rover1234` por defecto).
2. Levanta un servidor web en `http://192.168.4.1/`.
3. La página muestra un D-pad (▲◀■▶▼) para avanzar, retroceder, girar a la
   izquierda/derecha (giro en el lugar) o detenerse. También se puede manejar
   con las flechas o WASD del teclado.
4. Si deja de recibir comandos de movimiento por más de 800 ms (por ejemplo por
   un corte de WiFi), el firmware frena los motores solo (watchdog de seguridad).

## Cableado del puente H L298N

| L298N | ESP8266 |
|-------|---------|
| IN1 (motor izquierdo) | D1 (GPIO5)  |
| IN2 (motor izquierdo) | D2 (GPIO4)  |
| IN3 (motor derecho)   | D6 (GPIO12) |
| IN4 (motor derecho)   | D7 (GPIO13) |
| ENA, ENB              | puenteados a 5V (siempre habilitados) |
| GND                   | GND del ESP8266 (y de la fuente de los motores) |

Si algún motor gira al revés, es más simple invertir el cableado físico de ese
motor en el L298N que tocar el firmware; también se puede arreglar cambiando
los `#define MOTOR_..._IN1/IN2` en el código.

## Estructura

```
esp8266_motor_control/
├── main/
│   └── main.ino        # sketch (todo el firmware en un solo archivo)
├── build.sh             # compila con arduino-cli
├── flash.sh             # flashea por USB con esptool
├── monitor.sh           # abre el monitor serie
├── install_venv.sh      # crea el venv local con esptool/pyserial
└── README.md
```

## Requisitos

- Placa **ESP8266** (NodeMCU 1.0 / ESP-12E, o Wemos D1 Mini — mismo mapeo D1..D8).
- No se necesitan librerías externas: `ESP8266WiFi.h` y `ESP8266WebServer.h`
  vienen incluidas en el core de esp8266.
- Para compilar/flashear por línea de comandos (scripts de abajo): `arduino-cli`
  con el core `esp8266:esp8266` instalado, y Python 3 (para el venv de `esptool`).
- Alternativa: Arduino IDE con el paquete de placas **esp8266 by ESP8266 Community**
  instalado (Boards Manager → buscar "esp8266"), board **"NodeMCU 1.0 (ESP-12E Module)"**
  (o **"LOLIN(WEMOS) D1 R2 & mini"** si usás un Wemos D1 Mini).

## Compilar, flashear y monitorear (línea de comandos)

```bash
./build.sh              # compila el sketch con arduino-cli -> build/
./flash.sh               # auto-detecta el puerto y flashea build/main.ino.bin
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

En placas NodeMCU/Wemos D1 Mini el propio circuito USB-serie entra en modo
programación solo (no hace falta puentear ningún pin). Si usás un ESP-12/ESP-01
"pelado" con un adaptador FTDI externo, hay que mantener GPIO0 a GND durante el
reset para entrar en modo flasheo.

## Personalización

Al principio del archivo `.ino` están las constantes para ajustar:

```cpp
const char *AP_SSID     = "ROVER-MOTOR";
const char *AP_PASSWORD = "rover1234";   // mínimo 8 caracteres, o "" para red abierta
const int   AP_CHANNEL  = 1;
const int   AP_MAX_CONN = 4;
```

Y el tiempo del watchdog de seguridad:

```cpp
#define MOVE_TIMEOUT_MS 800   // ms sin recibir /move antes de frenar solo
```

## Uso

1. Flashear el sketch.
2. Alimentar el ESP8266 y el L298N/motores (el L298N necesita su propia fuente
   para los motores, no uses el 5V del ESP8266 para alimentarlos).
3. Desde el celular/PC, conectarse a la red WiFi `ROVER-MOTOR`.
4. Abrir el navegador en `http://192.168.4.1/`.
5. Usar el D-pad (o las flechas/WASD del teclado) para manejar el rover.

## Notas

- Este modo AP es ideal para controlar el rover sin depender de una red WiFi
  externa; si en el futuro se necesita que el ESP8266 se conecte a una red
  existente (modo estación) en lugar de crear la suya, hay que cambiar
  `WiFi.mode(WIFI_AP)` por `WIFI_STA` y usar `WiFi.begin(ssid, password)`.
- Este sketch no usa PWM/velocidad variable: cada motor va a fondo o detenido,
  porque ENA/ENB del L298N están puenteados a 5V. Si se agregan esos pines al
  ESP8266 para variar la velocidad, hay que generar PWM con `analogWrite()`
  sobre ENA/ENB (no sobre IN1-IN4).
