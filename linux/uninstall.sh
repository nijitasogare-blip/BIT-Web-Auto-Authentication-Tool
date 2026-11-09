#!/usr/bin/env bash
# 卸载 Linux 版 BIT-Web 自动登录
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin/bitweb"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/bitweb-autologin"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
UNIT="$UNIT_DIR/bitweb.service"
PURGE=0
[ "${1:-}" = "--purge" ] && PURGE=1

if command -v systemctl >/dev/null 2>&1; then
    systemctl --user stop bitweb.service 2>/dev/null || true
    systemctl --user disable bitweb.service 2>/dev/null || true
fi
if [ -f "$UNIT" ]; then
    rm -f "$UNIT"
    command -v systemctl >/dev/null 2>&1 && systemctl --user daemon-reload 2>/dev/null || true
fi
if [ -f "$BIN" ]; then
    rm -f "$BIN"
    echo "已删除 $BIN"
fi
if [ "$PURGE" = "1" ]; then
    rm -rf "$CONFIG_DIR"
    echo "已删除配置与日志：$CONFIG_DIR"
else
    echo "配置与日志保留：$CONFIG_DIR（使用 --purge 可删除）"
fi
echo "卸载完成。"