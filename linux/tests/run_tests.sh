#!/usr/bin/env bash
# Linux 本地集成测试：不依赖真实校园网
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"
WORK=/tmp/bwtest
rm -rf "$WORK"
mkdir -p "$WORK"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/bitweb-$(id -u)}/bitweb-autologin"
# 停掉可能已安装的 systemd 服务/残留实例，避免端口与锁冲突
systemctl --user stop bitweb 2>/dev/null || true
if [ -f "$RUNTIME_DIR/daemon.pid" ]; then kill -9 $(cat "$RUNTIME_DIR/daemon.pid") 2>/dev/null || true; rm -f "$RUNTIME_DIR/daemon.pid"; fi

echo "==> 构建"
make -s

echo "==> 协议向量"
./bitweb crypto-test

echo "==> 启动假门户"
python3 tests/fake_portal.py 18080 2024000001 'TestP@ss!234' "$WORK/portal_requests.log" >"$WORK/portal.out" 2>&1 &
FPID=$!
cleanup() {
  kill "$FPID" 2>/dev/null || true
  [ -n "${DPID:-}" ] && kill "$DPID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT
sleep 1

echo "==> 生成测试配置"
python3 tests/make_config.py "$WORK/config.json" 127.0.0.1:18080 2024000001 'TestP@ss!234' BIT-Web "http://127.0.0.1:18080/probe"
python3 tests/make_config.py "$WORK/config_sleep.json" 127.0.0.1:18080 2024000001 'TestP@ss!234' BIT-Web "http://www.baidu.com/"
python3 tests/make_config.py "$WORK/config_reconnect.json" 127.0.0.1:18080 2024000001 'TestP@ss!234' BIT-Web "http://www.baidu.com/"

echo "==> 登录请求测试（fake portal 校验签名）"
./bitweb --config "$WORK/config.json" login-test | tee "$WORK/login.txt"
grep -q '登录请求成功' "$WORK/login.txt"

echo "==> SSID 模拟 + 状态（白名单网络）"
BITWEB_SIMULATE_SSID=BIT-Web ./bitweb --config "$WORK/config.json" status | tee "$WORK/status.txt"
grep -q '监听状态  : 工作中' "$WORK/status.txt"

stop_daemon() {
    local pidfile="$RUNTIME_DIR/daemon.pid"
    if [ -f "$pidfile" ]; then
        local pid
        pid=$(cat "$pidfile" 2>/dev/null || true)
        [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null || true
    fi
    [ -n "${DPID:-}" ] && kill -TERM "$DPID" 2>/dev/null || true
    sleep 1
    if [ -f "$pidfile" ]; then
        local pid
        pid=$(cat "$pidfile" 2>/dev/null || true)
        [ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null || true
    fi
    [ -n "${DPID:-}" ] && kill -KILL "$DPID" 2>/dev/null || true
    sleep 1
    rm -f "$pidfile"
}

echo "==> 守护进程：检测到白名单并尝试认证"
BITWEB_SIMULATE_SSID=BIT-Web ./bitweb --config "$WORK/config.json" daemon >"$WORK/daemon.out" 2>&1 &
DPID=$!
sleep 9
grep -q '认证接口返回成功' "$WORK/log.txt" || { echo 'daemon login test failed'; cat "$WORK/log.txt"; exit 1; }
grep -q '守护进程启动' "$WORK/log.txt"
stop_daemon

echo "==> reconnect 信号"
BITWEB_SIMULATE_SSID=BIT-Web ./bitweb --config "$WORK/config_reconnect.json" daemon >"$WORK/daemon2.out" 2>&1 &
DPID=$!
sleep 2
./bitweb --config "$WORK/config_reconnect.json" reconnect
sleep 3
grep -q '收到立即检查请求' "$WORK/log.txt" || { echo 'reconnect test failed'; tail -20 "$WORK/log.txt"; exit 1; }
stop_daemon

echo "==> 非白名单网络：只读检测、不访问门户"
BEFORE=$(wc -l < "$WORK/portal_requests.log" 2>/dev/null || echo 0)
BITWEB_SIMULATE_SSID=SomeOtherWiFi ./bitweb --config "$WORK/config_sleep.json" daemon >"$WORK/daemon3.out" 2>&1 &
DPID=$!
sleep 6
grep -q '非白名单网络连通性（仅检测，不认证）' "$WORK/log.txt" || { echo 'idle probe test failed'; cat "$WORK/log.txt"; exit 1; }
AFTER=$(wc -l < "$WORK/portal_requests.log" 2>/dev/null || echo 0)
stop_daemon
if [ "$AFTER" != "$BEFORE" ]; then
    echo "FAIL: 非白名单网络访问了门户（$BEFORE -> $AFTER）"
    tail -5 "$WORK/portal_requests.log"
    exit 1
fi

echo
echo "全部 Linux 集成测试通过。"