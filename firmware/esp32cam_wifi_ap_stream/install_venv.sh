#!/usr/bin/env bash
# =============================================================================
# instalación del venv local de esp32cam_wifi_ap_stream
# -----------------------------------------------------------------------------
# Crea un entorno virtual Python (`.venv/`) e instala `esptool` y
# `pyserial` (para el monitor) en él. Los demás scripts (`flash.sh`,
# `monitor.sh`) usan este venv.
#
# Uso:
#   ./install_venv.sh              # crea/usa el venv e instala dependencias
#   ./install_venv.sh --force      # fuerza reinstalación
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
PYTHON="${PYTHON:-python3}"
FORCE=false
[ "${1:-}" = "--force" ] && FORCE=true

echo "==> Entorno virtual: $VENV_DIR"

# --- Crear venv si no existe -----------------------------------------------
if [ ! -d "$VENV_DIR" ]; then
  echo "==> Creando entorno virtual con $PYTHON..."
  "$PYTHON" -m venv "$VENV_DIR"
fi

PIP="$VENV_DIR/bin/pip"
"$VENV_DIR/bin/python" -m pip --version >/dev/null 2>&1 || {
  echo "❌ pip no disponible en el venv. Revisa la instalación de Python ($PYTHON)."
  exit 1
}

# --- Instalar/actualizar esptool + pyserial ---------------------------------
if "$VENV_DIR/bin/python" -c "import esptool, serial" >/dev/null 2>&1 && [ "$FORCE" = false ]; then
  echo "✔ esptool/pyserial ya instalados:"
  "$VENV_DIR/bin/esptool" version 2>/dev/null || "$VENV_DIR/bin/esptool.py" version
  exit 0
fi

echo "==> Instalando esptool y pyserial en el venv..."
"$PIP" install --upgrade pip
"$PIP" install esptool pyserial

echo
echo "==> Verificando instalación..."
if "$VENV_DIR/bin/python" -c "import esptool, serial" >/dev/null 2>&1; then
  echo "✅ esptool/pyserial instalados correctamente en $VENV_DIR"
  "$VENV_DIR/bin/esptool" version 2>/dev/null || "$VENV_DIR/bin/esptool.py" version
else
  echo "❌ Falló la instalación."
  exit 1
fi
