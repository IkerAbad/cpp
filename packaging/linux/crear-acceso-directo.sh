#!/bin/sh
# Crea el acceso directo del juego en el menú de aplicaciones (solo para tu usuario).
set -eu
dir=$(cd "$(dirname "$0")" && pwd)
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
cat > "$apps/reinos.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Reinos (nombre provisional)
Comment=Estrategia en tiempo real con logística
Exec="$dir/reinos"
Path=$dir
Terminal=false
Categories=Game;StrategyGame;
EOF
echo "Acceso directo creado en $apps/reinos.desktop"
