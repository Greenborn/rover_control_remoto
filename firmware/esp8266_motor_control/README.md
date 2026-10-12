# ESP8266 — Access Point con control de motores (sin cámara)

Este sketch hace que un módulo **ESP8266** (NodeMCU / Wemos D1 Mini), al encenderse,
cree su propia red WiFi (modo *Access Point*) y sirva una página web con un
joystick virtual (estilo juego de carreras) o, si se prefiere, un D-pad clásico
para manejar un rover de tracción diferencial a través de un puente H **L298N**,
cuyos pines IN1-IN4 se manejan a través de un expansor de I/O **PCF8574** por el
bus I2C (no se conectan directo a GPIOs del ESP8266).
No necesita router ni conexión a internet: te conectás directamente al ESP8266
desde el celular o la PC. No tiene cámara (para eso ver `../esp32cam_wifi_ap_stream`).

## Qué hace

1. Al bootear, crea la red WiFi `rover-tito` (contraseña `rover1234` por defecto).
2. Levanta un servidor web en `http://192.168.4.1/`.
3. La página muestra un joystick virtual (arrastrar con el dedo o el mouse) que
   da velocidad progresiva por rueda - avanzar + doblar traza una curva abierta,
   no un giro en el lugar. Con el botón ⚙ (opciones) se puede cambiar a un D-pad
   clásico (▲◀■▶▼, giro en el lugar) y también invertir los controles. Ambos
   modos se pueden manejar con el mouse/dedo o con las flechas/WASD del teclado.
   La elección de modo e inversión queda guardada en el navegador.
4. Si deja de recibir comandos de movimiento por más de 500 ms (por ejemplo por
   un corte de WiFi, o por alejarse del alcance del AP), el firmware frena los
   motores solo (watchdog de seguridad) - no hace falta salir a perseguirlo.
5. Si el expansor PCF8574 no responde por I2C (desconectado, mal cableado o sin
   alimentación), la página muestra un aviso en rojo arriba de todo.
6. Al arrancar (si detecta el PCF8574), hace un autotesteo: activa IN1, IN2,
   IN3 e IN4 una por una, 250 ms cada una, para verificar a simple vista
   (motor que se mueve, o led de la salida en el L298N) que cada canal
   responde. El detalle se puede ver por el monitor serie (115200 baud).

## Cableado: ESP8266 → PCF8574 → L298N

El ESP8266 maneja el PCF8574 por I2C, y el PCF8574 maneja los IN1-IN4 del L298N:

| PCF8574 | ESP8266 |
|---------|---------|
| SDA     | D2 (GPIO4) |
| SCL     | D1 (GPIO5) |
| VCC     | 3V3 o 5V (según el módulo PCF8574) |
| GND     | GND del ESP8266 |
| A0, A1, A2 | según jumpers del módulo (ver nota abajo) |

| L298N | PCF8574 |
|-------|---------|
| IN1 (motor izquierdo) | P0 |
| IN2 (motor izquierdo) | P1 |
| IN3 (motor derecho)   | P2 |
| IN4 (motor derecho)   | P3 |
| ENA, ENB              | puenteados a 5V (siempre habilitados) |
| GND                   | GND común (ESP8266, PCF8574 y fuente de los motores) |

Si algún motor gira al revés, es más simple invertir el cableado físico de ese
motor en el L298N que tocar el firmware; también se puede arreglar cambiando
los `#define MOTOR_..._IN1/IN2` en el código (bits P0-P3 del PCF8574).

Si el módulo PCF8574 tiene los pines A0/A1/A2 puenteados distinto (o es un
PCF8574A), hay que ajustar `PCF8574_ADDR` en `main.ino` a la dirección I2C que
corresponda.

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
- No se necesitan librerías externas: `ESP8266WiFi.h`, `ESP8266WebServer.h` y
  `Wire.h` vienen incluidas en el core de esp8266.
- Módulo expansor **PCF8574** (I2C) para manejar los IN1-IN4 del L298N.
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
const char *AP_SSID     = "rover-tito";
const char *AP_PASSWORD = "rover1234";   // mínimo 8 caracteres, o "" para red abierta
const int   AP_CHANNEL  = 1;
const int   AP_MAX_CONN = 4;
```

Y el tiempo del watchdog de seguridad:

```cpp
#define MOVE_TIMEOUT_MS 500   // ms sin recibir /drive antes de frenar solo
```

Y el PWM por software (ver "Notas" más abajo):

```cpp
#define PWM_PERIOD_MS    20   // período del PWM (50Hz)
#define PWM_MIN_DUTY_PCT 30   // duty mínimo para que el motor arranque
```

## Uso

1. Flashear el sketch.
2. Alimentar el ESP8266 y el L298N/motores (el L298N necesita su propia fuente
   para los motores, no uses el 5V del ESP8266 para alimentarlos).
3. Desde el celular/PC, conectarse a la red WiFi `rover-tito`.
4. Abrir el navegador en `http://192.168.4.1/`.
5. Usar el joystick (o las flechas/WASD del teclado) para manejar el rover.
   Tocar el botón ⚙ arriba a la derecha para cambiar a D-pad clásico o
   invertir los controles.

## Notas

- Este modo AP es ideal para controlar el rover sin depender de una red WiFi
  externa; si en el futuro se necesita que el ESP8266 se conecte a una red
  existente (modo estación) en lugar de crear la suya, hay que cambiar
  `WiFi.mode(WIFI_AP)` por `WIFI_STA` y usar `WiFi.begin(ssid, password)`.
- ENA/ENB del L298N están puenteados a 5V (siempre habilitados): no hay PWM de
  hardware. La velocidad progresiva por rueda (necesaria para las curvas
  abiertas del joystick) se logra con PWM por software, prendiendo y apagando
  IN1/IN2 de cada rueda a 50Hz vía el PCF8574 (ver `updateMotorPWM()` en
  `main.ino`). Si en algún momento se quiere PWM "de verdad" (más fino, sin
  pasar por I2C), se puede cortar el jumper ENA/ENB de la placa L298N y
  cablearlos a 2 GPIO libres del ESP8266 con `analogWrite()`, dejando IN1-IN4
  del PCF8574 solo como dirección fija.
