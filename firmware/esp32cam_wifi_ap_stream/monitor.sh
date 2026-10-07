#!/usr/bin/env bash
# =============================================================================
# monitor de puerto serie del firmware ESP32-CAM (esp32cam_wifi_ap_stream)
# -----------------------------------------------------------------------------
# Abre el terminal serie para ver el log en vivo del dispositivo. Usa el
# `pyserial-miniterm` del venv local (`install_venv.sh` lo crea junto con
# esptool). Para salir del monitor pulsa Ctrl-] .
#
# Uso:
#   ./monitor.sh                          # auto-detecta puerto a 115200
#   ./monitor.sh --port /dev/ttyUSB0      # puerto explícito
#   ./monitor.sh --baud 115200            # velocidad serie
#   ./monitor.sh -h | --help              # esta ayuda
#
# Nota: Ctrl-] cierra el monitor sin matar la terminal.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Opciones por defecto ----------------------------------------------------
PORT=""
BAUD="115200"      # coincide con Serial.begin(115200) del firmware

# --- Parseo de argumentos ----------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="${2:?--port requiere valor}"; shift 2 ;;
    --baud) BAUD="${2:?--baud requiere valor}"; shift 2 ;;
    -h|--help)
      grep '^#' "$0" | sed -n '2,18p' | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "❌ Opción desconocida: $1"; exit 1 ;;
  esac
done

# --- Dependencias: venv + pyserial-miniterm -----------------------------------
MINITERM="$SCRIPT_DIR/.venv/bin/pyserial-miniterm"
[ -x "$MINITERM" ] || MINITERM="$SCRIPT_DIR/.venv/bin/pyserial-miniterm.py"

if [ ! -x "$MINITERM" ]; then
  echo "⚠ No se encontró pyserial-miniterm en el venv local."
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
  echo "   ./monitor.sh --port /dev/ttyUSB0"
  exit 1
fi
echo "==> Puerto: $PORT | Baud: $BAUD"

# --- Lanzar el monitor --------------------------------------------------------
# --exit-char=29  -> Ctrl-] cierra el monitor sin matar la terminal
# --quiet         -> no mezclar los menús de pyserial con el log del dispositivo
echo "==> Conectando... (pulsa Ctrl-] para salir)"
exec "$MINITERM" --quiet --exit-char=29 "$PORT" "$BAUD"
