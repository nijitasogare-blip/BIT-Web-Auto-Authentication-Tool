#!/usr/bin/env bash
# 安装 Linux 版 BIT-Web 自动登录（用户级，无需 root）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin/bitweb"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/bitweb-autologin"
CONFIG="$CONFIG_DIR/config.json"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
UNIT="$UNIT_DIR/bitweb.service"

echo "==> 构建"
make -C "$HERE"

echo "==> 安装到 $BIN"
install -d "$PREFIX/bin" "$CONFIG_DIR/logs" "$UNIT_DIR"
install -m 0755 "$HERE/bitweb" "$BIN"
if [ ! -f "$CONFIG" ]; then
    install -m 0600 "$HERE/config.example.json" "$CONFIG"
    echo "已生成配置：$CONFIG"
else
    echo "已存在配置，保留：$CONFIG"
fi

echo "==> 安装 systemd 用户服务"
sed -e "s|@BIN@|$BIN|g" -e "s|@CONFIG@|$CONFIG|g" -e "s|@DOC@|$HERE/README.md|g" \
    "$HERE/systemd/bitweb.service.in" > "$UNIT"
if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload 2>/dev/null || true
    systemctl --user enable --now bitweb.service 2>/dev/null && \
        echo "已启用并启动 bitweb.service" || \
        echo "systemd 用户服务不可用；可手动运行：$BIN daemon --config \"$CONFIG\" &"
else
    echo "未找到 systemctl；请手动运行：$BIN daemon --config \"$CONFIG\" &"
fi

echo
echo "完成。下一步："
echo "  $BIN set-password"
echo "  $BIN status"
echo "  $BIN tui"
echo
echo "提示：SSID 检测依赖 NetworkManager（nmcli）；密码保存在 0600 权限的 $CONFIG。"