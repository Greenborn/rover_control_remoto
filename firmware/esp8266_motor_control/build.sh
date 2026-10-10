#!/usr/bin/env bash
# =============================================================================
# compilación del firmware ESP8266 (esp8266_motor_control)
# -----------------------------------------------------------------------------
# Usa `arduino-cli` nativo con compilación paralela (`--jobs`), aprovechando
# todos los núcleos de CPU. Si `arduino-cli` no está disponible, se sugiere su
# instalación (ver nota al final).
#
# Uso:
#   ./build.sh              # compila con todos los núcleos (auto)
#   ./build.sh --jobs 8     # fija el número de tareas paralelas
#   ./build.sh --serial     # compila en serie (--jobs 1)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH_DIR="$SCRIPT_DIR/main"
BUILD_DIR="$SCRIPT_DIR/build"

# Placa: NodeMCU 1.0 (ESP-12E Module). También sirve para un Wemos D1 Mini
# (mismo mapeo D1..D8); si usás esa placa, cambiá el FQBN a
# "esp8266:esp8266:d1_mini".
FQBN="esp8266:esp8266:nodemcuv2"

# Número de tareas paralelas: 0 = todos los núcleos disponibles
JOBS=0
while [ $# -gt 0 ]; do
  case "$1" in
    --jobs) JOBS="${2:?--jobs requiere valor}"; shift 2 ;;
    --serial) JOBS=1; shift ;;
    -h|--help) grep '^#' "$0" | sed -n '2,13p' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "❌ Opción desconocida: $1"; exit 1 ;;
  esac
done

# Localizar arduino-cli (preferimos el nativo sobre el snap confinado)
ARDUINO_CLI="${ARDUINO_CLI:-}"
if [ -z "$ARDUINO_CLI" ]; then
  for cand in "$SCRIPT_DIR/.venv/bin/arduino-cli" "$HOME/.local/bin/arduino-cli" /usr/local/bin/arduino-cli /usr/bin/arduino-cli; do
    if [ -x "$cand" ]; then ARDUINO_CLI="$cand"; break; fi
  done
fi

if [ -z "$ARDUINO_CLI" ]; then
  echo "❌ No se encontró arduino-cli."
  echo "   Instálalo (nativo, NO el snap) en ~/.local/bin:"
  echo "     curl -L -o /tmp/arduino-cli.tar.gz https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz"
  echo "     tar xzf /tmp/arduino-cli.tar.gz arduino-cli"
  echo "     mkdir -p ~/.local/bin && mv arduino-cli ~/.local/bin/"
  echo "   Luego confirma que ve el core esp8266:"
  echo "     ~/.local/bin/arduino-cli core list"
  exit 1
fi

# Verificar que arduino-cli vea el core esp8266
if ! "$ARDUINO_CLI" core list 2>/dev/null | grep -q "esp8266:esp8266"; then
  echo "❌ arduino-cli no tiene instalado el core esp8266:esp8266."
  echo "   Instálalo (usa el mismo data-dir ~/.arduino15):"
  echo "     $ARDUINO_CLI core update-index"
  echo "     $ARDUINO_CLI core install esp8266:esp8266"
  exit 1
fi

echo "==> Compilando sketch: $SKETCH_DIR"
echo "==> FQBN: $FQBN"
echo "==> Tareas paralelas: $([ "$JOBS" -eq 0 ] && echo "todos los núcleos ($(nproc))" || echo "$JOBS")"

# Limpiar build anterior
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

"$ARDUINO_CLI" compile \
  --fqbn "$FQBN" \
  --jobs "$JOBS" \
  --build-path "$BUILD_DIR" \
  "$SKETCH_DIR/main.ino"

echo
echo "Build OK -> $BUILD_DIR"
echo "  App: $BUILD_DIR/main.ino.bin"
