#!/usr/bin/env bash
# =============================================================================
# Flasheo del firmware ESP8266 (esp8266_motor_control)
# =============================================================================
# Usa el `esptool` del venv local (`install_venv.sh` lo crea). Flashea por USB
# el binario generado por build.sh (`main.ino.bin`) en la dirección 0x0 (a
# diferencia del ESP32, el core esp8266 genera un único binario que ya incluye
# todo lo necesario, sin bootloader/partition table por separado).
#
# Este sketch no guarda nada persistente (SSID/password del AP están fijos
# en el código, no hay SPIFFS en uso), así que no hace falta elegir entre
# "preservar config" o "wipe": el único modo extra es `--erase`, que borra
# toda la flash antes de escribir.
#
# Recordatorio de hardware: en placas NodeMCU / Wemos D1 Mini el propio
# USB-serie de la placa entra en modo programación solo (no hace falta
# puentear ningún pin). Si en cambio usás un ESP-12/ESP-01 "pelado" con un
# adaptador FTDI externo, hay que mantener GPIO0 a GND durante el reset para
# entrar en modo flasheo.
#
# Uso:
#   ./flash.sh                          # auto-detecta puerto, flashea build/main.ino.bin
#   ./flash.sh --port /dev/ttyUSB0      # puerto explícito
#   ./flash.sh --erase                  # borra toda la flash antes de flashear
#   ./flash.sh --baud 115200            # velocidad serie personalizada
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# --- Opciones por defecto ----------------------------------------------------
PORT=""
BAUD="460800"
ERASE=false
VERIFY=true

# --- Parseo de argumentos ----------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --port)    PORT="${2:?--port requiere valor}"; shift 2 ;;
    --baud)    BAUD="${2:?--baud requiere valor}"; shift 2 ;;
    --erase)   ERASE=true; shift ;;
    --no-verify) VERIFY=false; shift ;;
    -h|--help)
      grep '^#' "$0" | sed -n '2,22p' | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "❌ Opción desconocida: $1"; exit 1 ;;
  esac
done

# --- Dependencias: venv + esptool --------------------------------------------
VENV_PY="$SCRIPT_DIR/.venv/bin/python"
ESPTOOL="$SCRIPT_DIR/.venv/bin/esptool"
[ -x "$ESPTOOL" ] || ESPTOOL="$SCRIPT_DIR/.venv/bin/esptool.py"

if [ ! -x "$ESPTOOL" ]; then
  echo "⚠ No se encontró esptool en el venv local."
  echo "  Instalándolo automáticamente con install_venv.sh..."
  bash "$SCRIPT_DIR/install_venv.sh"
fi

# --- Auto-detección de puerto -------------------------------------------------
if [ -z "$PORT" ]; then
  for cand in /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyUSB1 /dev/ttyACM1; do
    if [ -e "$cand" ]; then PORT="$cand"; break; fi
  done
fi

if [ -z "$PORT" ]; then
  echo "❌ No se detectó ningún puerto serie (/dev/ttyUSB* o /dev/ttyACM*)."
  echo "   Conecta la placa por USB y vuelve a intentar, o indica el puerto:"
  echo "   ./flash.sh --port /dev/ttyUSB0"
  exit 1
fi
echo "==> Puerto: $PORT"

# --- Comprobación de binarios --------------------------------------------------
if [ ! -f "$BUILD_DIR/main.ino.bin" ]; then
  echo "❌ No existe $BUILD_DIR/main.ino.bin. Compila primero:"
  echo "   bash $SCRIPT_DIR/build.sh"
  exit 1
fi

# --- Construcción del comando --------------------------------------------------
ESPTOOL_FLASH_MODE="dio"
FLASH_FREQ="40m"
FLASH_SIZE="4MB"
CHIP="esp8266"

common_args=(--chip "$CHIP" --port "$PORT" --baud "$BAUD")
flash_opts=(--flash-mode "$ESPTOOL_FLASH_MODE" --flash-freq "$FLASH_FREQ" --flash-size "$FLASH_SIZE")

if [ "$VERIFY" = false ]; then
  flash_opts+=(--no-verify)
fi

# --- Wipe total opcional -------------------------------------------------------
if [ "$ERASE" = true ]; then
  echo "==> Borrando toda la flash..."
  "$VENV_PY" -m esptool "${common_args[@]}" erase_flash
fi

# --- Flasheo --------------------------------------------------------------------
echo "==> Flasheando..."
echo ""

"$VENV_PY" -m esptool "${common_args[@]}" \
  write_flash "${flash_opts[@]}" 0x0 "$BUILD_DIR/main.ino.bin"

echo
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║              ✅ Flash completado exitosamente                  ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "📊 Resumen:"
echo "   Puerto: $PORT"
echo "   Velocidad: $BAUD baud"
echo "   Modo: $([ "$ERASE" = true ] && echo "erase + flash" || echo "flash")"
echo ""
echo "➡️  Resetear la placa (o desconectar/reconectar USB) para arrancar el firmware."
echo "   Luego buscar la red WiFi \"rover-tito\" y abrir http://192.168.4.1/"
echo ""
