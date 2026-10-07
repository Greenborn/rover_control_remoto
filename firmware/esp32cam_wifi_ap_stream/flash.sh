#!/usr/bin/env bash
# =============================================================================
# Flasheo del firmware ESP32-CAM (esp32cam_wifi_ap_stream)
# =============================================================================
# Usa el `esptool` del venv local (`install_venv.sh` lo crea). Flashea por USB
# la imagen completa generada por build.sh (`main.ino.merged.bin`) o, con
# `--partial`, los binarios por separado en sus direcciones.
#
# Este sketch no guarda nada persistente (SSID/password del AP están fijos
# en el código, no hay NVS ni SPIFFS en uso), así que no hace falta elegir
# entre "preservar config" o "wipe": el único modo extra es `--erase`, que
# borra toda la flash antes de escribir (útil si la placa viene con datos
# de fábrica que confunden al bootloader).
#
# Recordatorio de hardware: el ESP32-CAM no tiene USB propio, se flashea con
# un adaptador FTDI/USB-TTL y hay que puentear GPIO0 a GND antes de resetear
# o alimentar la placa para entrar en modo programación (ver README.md).
#
# Uso:
#   ./flash.sh                          # auto-detecta puerto, flashea imagen completa
#   ./flash.sh --port /dev/ttyUSB0      # puerto explícito
#   ./flash.sh --erase                  # borra toda la flash antes de flashear
#   ./flash.sh --partial                # flashea los 4 binarios por separado
#   ./flash.sh --baud 921600            # velocidad serie personalizada
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# --- Opciones por defecto ----------------------------------------------------
PORT=""
BAUD="460800"
ERASE=false
FLASH_MODE="merged" # merged | partial
VERIFY=true

# --- Parseo de argumentos ----------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --port)    PORT="${2:?--port requiere valor}"; shift 2 ;;
    --baud)    BAUD="${2:?--baud requiere valor}"; shift 2 ;;
    --erase)   ERASE=true; shift ;;
    --partial) FLASH_MODE="partial"; shift ;;
    --no-verify) VERIFY=false; shift ;;
    -h|--help)
      grep '^#' "$0" | sed -n '2,24p' | sed 's/^# \{0,1\}//'
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
  echo "   Conecta el adaptador FTDI/USB-TTL y vuelve a intentar, o indica el puerto:"
  echo "   ./flash.sh --port /dev/ttyUSB0"
  exit 1
fi
echo "==> Puerto: $PORT"

# --- Comprobación de binarios --------------------------------------------------
if [ ! -d "$BUILD_DIR" ]; then
  echo "❌ No existe $BUILD_DIR. Compila primero:"
  echo "   bash $SCRIPT_DIR/build.sh"
  exit 1
fi

# --- Construcción del comando --------------------------------------------------
ESPTOOL_FLASH_MODE="dio"
FLASH_FREQ="40m"
FLASH_SIZE="4MB"
CHIP="esp32"

common_args=(--chip "$CHIP" --port "$PORT" --baud "$BAUD")
# Nota: en esptool v5 los flags de flash van DESPUÉS del subcomando write-flash
flash_opts=(--flash-mode "$ESPTOOL_FLASH_MODE" --flash-freq "$FLASH_FREQ" --flash-size "$FLASH_SIZE")

if [ "$VERIFY" = false ]; then
  flash_opts+=(--no-verify)
fi

echo ""
echo "⚠️  Recordatorio: para entrar en modo programación, puentear GPIO0 a GND"
echo "   antes de resetear/alimentar el ESP32-CAM. Al terminar, desconectar el"
echo "   puente y resetear para que arranque normalmente."
echo ""

# --- Wipe total opcional -------------------------------------------------------
if [ "$ERASE" = true ]; then
  echo "==> Borrando toda la flash..."
  "$VENV_PY" -m esptool "${common_args[@]}" erase_flash
fi

# --- Flasheo --------------------------------------------------------------------
echo "==> Flasheando..."
echo ""

if [ "$FLASH_MODE" = "partial" ]; then
  "$VENV_PY" -m esptool "${common_args[@]}" \
    write_flash "${flash_opts[@]}" \
    0x1000  "$BUILD_DIR/main.ino.bootloader.bin" \
    0x8000  "$BUILD_DIR/main.ino.partitions.bin" \
    0xe000  "$BUILD_DIR/boot_app0.bin" \
    0x10000 "$BUILD_DIR/main.ino.bin"
else
  "$VENV_PY" -m esptool "${common_args[@]}" \
    write_flash "${flash_opts[@]}" 0x0 "$BUILD_DIR/main.ino.merged.bin"
fi

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
echo "➡️  Quitar el puente GPIO0-GND y resetear la placa para arrancar el firmware."
echo "   Luego buscar la red WiFi \"ROVER-CAM\" y abrir http://192.168.4.1/"
echo ""
