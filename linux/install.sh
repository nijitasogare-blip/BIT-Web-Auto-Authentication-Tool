#!/usr/bin/env bash
# 安装 Linux 版 BIT-Web 自动登录（用户级，无需 root）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin/bitweb"
GUI_BIN="$PREFIX/bin/bitweb-gui"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/bitweb-autologin"
CONFIG="$CONFIG_DIR/config.json"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
UNIT="$UNIT_DIR/bitweb.service"
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
AUTOSTART_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"

echo "==> 构建 CLI/守护进程"
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

# 可选：GTK3 桌面面板 + 托盘
if pkg-config --exists gtk+-3.0 ayatana-appindicator3-0.1 2>/dev/null; then
    echo "==> 构建 GTK 桌面面板"
    make -C "$HERE" bitweb-gui
    install -m 0755 "$HERE/bitweb-gui" "$GUI_BIN"
    install -d "$APPS_DIR" "$AUTOSTART_DIR"
    cat > "$APPS_DIR/bitweb.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=BIT-Web 自动登录
Comment=BIT-Web 断线自动重连控制面板
Exec=$GUI_BIN
Icon=network-wireless
Terminal=false
Categories=Utility;Network;
EOF
    cat > "$AUTOSTART_DIR/bitweb-gui.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=BIT-Web 自动登录托盘
Exec=$GUI_BIN --silent
Icon=network-wireless
Terminal=false
X-GNOME-Autostart-enabled=true
EOF
    echo "已安装桌面面板：$GUI_BIN（开始菜单 / 登录后托盘自动启动）"
else
    echo "未找到 GTK3/AppIndicator 开发包，跳过桌面面板（CLI/TUI 照常可用）。"
    echo "如需 GUI：sudo apt install libgtk-3-dev libayatana-appindicator3-dev"
fi

echo
echo "完成。下一步："
echo "  $BIN set-password"
echo "  $BIN status"
echo "  $BIN tui"
[ -x "$GUI_BIN" ] && echo "  $GUI_BIN        # GTK 桌面面板"
echo
echo "提示：SSID 检测依赖 NetworkManager（nmcli）；密码保存在 0600 权限的 $CONFIG。"