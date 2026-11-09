#!/usr/bin/env bash
# 在 Xvfb 中启动 GTK 面板并截图（CI/无桌面环境验证用）
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"
CONFIG="${1:-$HOME/.config/bitweb-autologin/config.json}"
unset WAYLAND_DISPLAY
export GDK_BACKEND=x11
OUT="${2:-/tmp/bitweb-gui.png}"
pkill -f 'bitweb-gui' 2>/dev/null || true
pkill -f 'Xvfb :99' 2>/dev/null || true
sleep 1
Xvfb :99 -screen 0 1280x800x24 >/tmp/xvfb.log 2>&1 &
XPID=$!
sleep 1
DISPLAY=:99 ./bitweb-gui --config "$CONFIG" >/tmp/bitweb-gui.log 2>&1 &
GPID=$!
trap 'kill $GPID $XPID 2>/dev/null || true' EXIT
sleep 3
echo "--- windows ---"
DISPLAY=:99 xdotool search --name '.*' getwindowname %@ 2>/dev/null | head -10 || true
echo "--- screenshot ---"
DISPLAY=:99 import -window root "$OUT"
file "$OUT"
echo "--- gui log ---"
head -10 /tmp/bitweb-gui.log