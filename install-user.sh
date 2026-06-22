#!/usr/bin/env bash
set -e

APP_NAME="thinmon"
BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
BIN_PATH="$BIN_DIR/$APP_NAME"
DESKTOP_FILE="$APP_DIR/$APP_NAME.desktop"

mkdir -p "$BIN_DIR"
mkdir -p "$APP_DIR"

cmake -S . -B build
cmake --build build -j

cp build/thinmon "$BIN_PATH"
chmod +x "$BIN_PATH"

cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=ThinMon
Comment=Small btop-style system monitor strip
Exec=env THINMON_SCREEN=1 THINMON_HEIGHT=60 $BIN_PATH
Icon=utilities-system-monitor
Terminal=false
Categories=System;Monitor;
StartupNotify=false
EOF

chmod +x "$DESKTOP_FILE"

kbuildsycoca6 >/dev/null 2>&1 || true

echo "Installed:"
echo "  $BIN_PATH"
echo "  $DESKTOP_FILE"
echo
echo "Open KDE launcher and search for: ThinMon"
