#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""统一状态采集：TUI / GUI / 命令行共用，输出人类可读文本或 JSON。

用法：
    python status_report.py                 # 打印状态文本
    python status_report.py --json          # 供 GUI 调用
    python status_report.py --no-network    # 不探测外网（更快）
    python status_report.py --tail 40       # 日志行数
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
import json
import re
import subprocess
import sys
from pathlib import Path

import appconfig
import dpapi
from bitweb_monitor import (
    check_connectivity,
    get_wlan_interfaces,
    ssid_allowed,
)

TASK_NAME = "BITWebAutoLogin"
TRAY_TASK_NAME = "BITWebAutoLoginTray"
DAEMON_MUTEX = "Local\\BITWebAutoLogin.Singleton"
TRAY_MUTEX = "Local\\BITWebAutoLoginTray"


def _mutex_exists(name: str) -> bool:
    if sys.platform != "win32":
        return False
    kernel32 = ctypes.windll.kernel32
    kernel32.OpenMutexW.restype = ctypes.c_void_p
    handle = kernel32.OpenMutexW(0x00100000, False, name)  # SYNCHRONIZE
    if not handle:
        return False
    kernel32.CloseHandle(handle)
    return True


def _run_hidden(cmd: list[str], timeout: float = 8.0) -> tuple[int, str]:
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            timeout=timeout,
            creationflags=0x08000000,
        )
    except (subprocess.TimeoutExpired, OSError):
        return -1, ""
    output = b""
    for encoding in ("mbcs", "utf-8", "gbk"):
        try:
            text = (proc.stdout or b"").decode(encoding)
            break
        except (UnicodeDecodeError, LookupError):
            continue
    else:
        text = (proc.stdout or b"").decode("utf-8", errors="replace")
    return proc.returncode, text


def _task_info(task_name: str) -> dict:
    info = {
        "registered": False,
        "status": "",
        "last_result": None,
        "last_run": "",
    }
    code, output = _run_hidden(["schtasks", "/query", "/tn", task_name, "/fo", "LIST", "/v"])
    if code != 0:
        return info
    info["registered"] = True
    for line in output.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        key_l = key.strip().lower()
        value = value.strip()
        if key_l in ("status", "状态", "模式"):
            info["status"] = value
        elif key_l in ("last result", "上次结果"):
            match = re.search(r"-?\d+", value)
            if match:
                info["last_result"] = int(match.group(0))
        elif key_l in ("last run time", "上次运行时间"):
            info["last_run"] = value
    return info


def _read_log_tail(log_path: Path, lines: int) -> list[str]:
    if not log_path.exists():
        return []
    try:
        with log_path.open("r", encoding="utf-8", errors="replace") as fh:
            content = fh.readlines()
        return [line.rstrip("\n") for line in content[-lines:]]
    except OSError:
        return []


def collect_status(
    config_path: Path,
    with_network: bool = True,
    tail_lines: int = 20,
    install_dir: Path | None = None,
) -> dict:
    config = appconfig.load_config(config_path)
    config_exists = config_path.exists()
    install_dir = install_dir or config_path.parent

    password_configured = False
    password_error = None
    try:
        password_configured = bool(
            dpapi.unprotect(str(config.get("password_dpapi") or "")).decode("utf-8")
        ) if config.get("password_dpapi") else bool(config.get("password"))
    except Exception as exc:  # noqa: BLE001
        password_error = str(exc)

    allowed = [str(s) for s in config["network"]["allowed_ssids"]]
    interfaces, wlan_error = get_wlan_interfaces()
    connected = [
        item for item in interfaces if item.get("connected") and item.get("ssid")
    ]
    current_ssid = ""
    in_whitelist = False
    for item in connected:
        if ssid_allowed(item["ssid"], allowed):
            current_ssid = item["ssid"]
            in_whitelist = True
            break

    connectivity = {"checked": False, "online": None, "detail": ""}
    if with_network and in_whitelist:
        online, detail = check_connectivity(config)
        connectivity = {"checked": True, "online": bool(online), "detail": detail}

    log_path = appconfig.resolve_relative(
        config_path, str(config["logging"].get("file") or "logs/bitweb_monitor.log")
    )

    return {
        "version": appconfig.APP_VERSION,
        "generated_at": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "install_dir": str(install_dir),
        "config_path": str(config_path),
        "config_exists": config_exists,
        "username": appconfig.mask_user(str(config.get("username") or "")),
        "password_configured": password_configured,
        "password_error": password_error,
        "allowed_ssids": allowed,
        "wlan": {
            "error": wlan_error,
            "interfaces": [
                {
                    "name": item.get("name", ""),
                    "ssid": item.get("ssid", ""),
                    "state": item.get("state", ""),
                    "connected": bool(item.get("connected")),
                }
                for item in interfaces
            ],
            "connected_ssids": [item["ssid"] for item in connected],
            "current_ssid": current_ssid,
            "in_whitelist": in_whitelist,
        },
        "mode": "active" if in_whitelist else "sleep",
        "connectivity": connectivity,
        "daemon": {"running": _mutex_exists(DAEMON_MUTEX)},
        "task": _task_info(TASK_NAME),
        "tray": {
            "running": _mutex_exists(TRAY_MUTEX),
            "task": _task_info(TRAY_TASK_NAME),
        },
        "log_path": str(log_path),
        "log_tail": _read_log_tail(log_path, tail_lines),
    }


def format_text(data: dict) -> str:
    lines: list[str] = []
    lines.append("BIT-Web 自动登录  v%s" % data.get("version", "?"))
    lines.append("生成时间  : %s" % data.get("generated_at", ""))
    lines.append("安装目录  : %s" % data.get("install_dir", ""))
    lines.append("用户名    : %s" % data.get("username", ""))
    if data.get("password_configured"):
        lines.append("密码      : 已配置（DPAPI 加密）")
    else:
        detail = data.get("password_error")
        lines.append("密码      : 未配置%s" % ("（%s）" % detail if detail else "（运行 set_password.py）"))
    lines.append("白名单    : %s" % "、".join(data.get("allowed_ssids") or []))

    wlan = data.get("wlan") or {}
    if wlan.get("error"):
        lines.append("WLAN      : 读取失败（%s）" % wlan["error"])
    elif not wlan.get("interfaces"):
        lines.append("WLAN      : 未发现无线网卡")
    else:
        for item in wlan["interfaces"]:
            lines.append(
                "WLAN      : %s -> %s（%s）"
                % (item.get("name") or "?", item.get("ssid") or "未连接", item.get("state") or "?")
            )
    lines.append(
        "监听状态  : %s%s"
        % (
            "工作中（白名单 SSID：%s）" % wlan.get("current_ssid", "")
            if wlan.get("in_whitelist")
            else "休眠（当前不是白名单 SSID）",
            "" if wlan.get("in_whitelist") else "，不访问门户",
        )
    )

    connectivity = data.get("connectivity") or {}
    if connectivity.get("checked"):
        lines.append(
            "外网连通  : %s（%s）"
            % ("正常" if connectivity.get("online") else "不通", connectivity.get("detail", ""))
        )
    elif not wlan.get("in_whitelist"):
        lines.append("外网连通  : 跳过（休眠中不发起任何网络请求）")

    lines.append("守护进程  : %s" % ("运行中" if data.get("daemon", {}).get("running") else "未运行"))
    task = data.get("task") or {}
    if task.get("registered"):
        lines.append(
            "计划任务  : %s（上次运行 %s，结果 %s）"
            % (task.get("status", "?"), task.get("last_run", "?"), task.get("last_result", "?"))
        )
    else:
        lines.append("计划任务  : 未注册")
    lines.append("托盘界面  : %s" % ("运行中" if data.get("tray", {}).get("running") else "未运行"))

    if data.get("log_tail"):
        lines.append("")
        lines.append("最近日志（%s）:" % data.get("log_path", ""))
        for line in data["log_tail"]:
            lines.append("  " + line)
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="BIT-Web 自动登录状态")
    parser.add_argument("--config", help="config.json 路径")
    parser.add_argument("--json", action="store_true", help="输出 JSON")
    parser.add_argument("--no-network", action="store_true", help="不探测外网")
    parser.add_argument("--tail", type=int, default=20, help="日志行数（默认 20）")
    args = parser.parse_args(argv)

    config_path = appconfig.resolve_config_path(args.config)
    data = collect_status(
        config_path=config_path,
        with_network=not args.no_network,
        tail_lines=max(0, args.tail),
    )
    if args.json:
        print(json.dumps(data, ensure_ascii=False, indent=2))
    else:
        print(format_text(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
