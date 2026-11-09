#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BIT-Web 自动登录 - 终端控制面板（TUI）。

在 Windows Terminal / cmd / PowerShell 中运行：
    python tui.py
    python tui.py --config "C:\\...\\config.json"
    python tui.py --print            # 只打印一次状态（供脚本/测试）
"""

from __future__ import annotations

import argparse
import ctypes
import os
import subprocess
import sys
from pathlib import Path

import appconfig
import status_report

_RESET = "\x1b[0m"
_BOLD = "\x1b[1m"
_CYAN = "\x1b[36m"
_GREEN = "\x1b[32m"
_YELLOW = "\x1b[33m"
_RED = "\x1b[31m"
_DIM = "\x1b[2m"

TASK_NAME = status_report.TASK_NAME


def _enable_ansi() -> None:
    if sys.platform != "win32":
        return
    try:
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)
    except Exception:  # noqa: BLE001
        pass


def _clear() -> None:
    os.system("cls" if sys.platform == "win32" else "clear")


def _line(text: str = "") -> None:
    print(text)


def _run_interactive(cmd: list[str]) -> int:
    try:
        return subprocess.call(cmd)
    except OSError as exc:
        print("无法执行命令: %s" % exc)
        return 1


def _capture(cmd: list[str], timeout: float = 60) -> str:
    try:
        proc = subprocess.run(
            cmd, capture_output=True, timeout=timeout, creationflags=0x08000000
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return "执行失败: %s" % exc
    data = (proc.stdout or b"") + (proc.stderr or b"")
    for encoding in ("utf-8", "mbcs", "gbk"):
        try:
            return data.decode(encoding)
        except (UnicodeDecodeError, LookupError):
            continue
    return data.decode("utf-8", errors="replace")


def _pythonw() -> str:
    candidate = Path(sys.executable).with_name("pythonw.exe")
    return str(candidate) if candidate.exists() else sys.executable


def _powershell() -> str:
    return os.path.join(
        os.environ.get("SystemRoot", r"C:\Windows"),
        r"System32\WindowsPowerShell\v1.0\powershell.exe",
    )


def _start_daemon(install_dir: Path, config_path: Path) -> None:
    code = _run_schtasks(["/run", "/tn", TASK_NAME])
    if code != 0:
        subprocess.Popen(
            [
                _pythonw(),
                str(install_dir / "bitweb_monitor.py"),
                "--config",
                str(config_path),
            ],
            cwd=str(install_dir),
            creationflags=0x08000000,
        )


def _run_schtasks(args: list[str]) -> int:
    try:
        proc = subprocess.run(
            ["schtasks"] + args,
            capture_output=True,
            creationflags=0x08000000,
        )
        return proc.returncode
    except OSError:
        return 1


def render(data: dict, config_path: Path) -> None:
    _clear()
    wlan = data.get("wlan") or {}
    connectivity = data.get("connectivity") or {}
    daemon = data.get("daemon") or {}
    task = data.get("task") or {}
    tray = data.get("tray") or {}

    print(_CYAN + _BOLD + "=" * 70 + _RESET)
    print(
        _CYAN
        + _BOLD
        + "  BIT-Web 自动登录 · 终端控制面板    v%s" % data.get("version", "?")
        + _RESET
    )
    print(_CYAN + _BOLD + "=" * 70 + _RESET)
    print("  配置文件 : %s" % config_path)
    print("  用户名   : %s" % data.get("username", ""))
    if data.get("password_configured"):
        print("  密码     : " + _GREEN + "已配置（DPAPI 加密）" + _RESET)
    else:
        print("  密码     : " + _YELLOW + "未配置（按 P 设置）" + _RESET)
    print("  白名单   : %s" % "、".join(data.get("allowed_ssids") or []))

    if wlan.get("in_whitelist"):
        print(
            "  监听状态 : "
            + _GREEN
            + "工作中（%s）" % wlan.get("current_ssid", "")
            + _RESET
        )
    else:
        print(
            "  监听状态 : "
            + _DIM
            + "休眠（当前：%s，不访问门户）"
            % ("、".join(wlan.get("connected_ssids") or []) or "未连接")
            + _RESET
        )
    if connectivity.get("checked"):
        if connectivity.get("online"):
            print("  外网连通 : " + _GREEN + "正常" + _RESET + "（%s）" % connectivity.get("detail", ""))
        else:
            print("  外网连通 : " + _RED + "不通" + _RESET + "（%s）" % connectivity.get("detail", ""))
    daemon_text = "运行中" if daemon.get("running") else "未运行"
    daemon_color = _GREEN if daemon.get("running") else _RED
    print("  守护进程 : %s%s%s" % (daemon_color, daemon_text, _RESET))
    if task.get("registered"):
        print(
            "  计划任务 : %s（上次结果 %s）"
            % (task.get("status") or "已注册", task.get("last_result"))
        )
    else:
        print("  计划任务 : " + _YELLOW + "未注册（重新运行 install.ps1）" + _RESET)
    print("  托盘界面 : %s" % ("运行中" if tray.get("running") else "未运行"))

    print(_DIM + "-" * 70 + _RESET)
    print(_BOLD + "  最近日志" + _RESET)
    tail = data.get("log_tail") or []
    if tail:
        for line in tail[-12:]:
            print("  " + line)
    else:
        print("  （暂无日志）")
    print(_DIM + "-" * 70 + _RESET)
    print(
        "  [R]刷新  [S]自检  [P]设置密码  [L]打开日志  "
        "[G]托盘界面  [D]重启守护进程  [O]安装目录  [Q]退出"
    )
    print(_DIM + "  请按键: " + _RESET, end="", flush=True)


def _pause() -> None:
    print()
    try:
        input("  按回车键返回...")
    except (EOFError, KeyboardInterrupt):
        pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="BIT-Web 自动登录 TUI")
    parser.add_argument("--config", help="config.json 路径")
    parser.add_argument("--print", action="store_true", help="只打印一次状态后退出")
    parser.add_argument("--tail", type=int, default=20)
    args = parser.parse_args(argv)

    config_path = appconfig.resolve_config_path(args.config)
    install_dir = Path(__file__).resolve().parent

    if args.print:
        data = status_report.collect_status(
            config_path, with_network=True, tail_lines=args.tail,
            install_dir=install_dir,
        )
        print(status_report.format_text(data))
        return 0

    if sys.platform != "win32":
        print("TUI 仅支持 Windows")
        return 1
    try:
        import msvcrt
    except ImportError:
        print("当前 Python 环境缺少 msvcrt，无法运行 TUI")
        return 1
    if not sys.stdin or not sys.stdin.isatty():
        print("请在交互式终端中运行 TUI（当前不是控制台）。")
        return 1

    _enable_ansi()
    while True:
        data = status_report.collect_status(
            config_path, with_network=True, tail_lines=args.tail,
            install_dir=install_dir,
        )
        render(data, config_path)
        key = msvcrt.getch()
        if key in (b"q", b"Q", b"\x1b"):
            _clear()
            return 0
        if key in (b"r", b"R", b"\r"):
            continue
        if key in (b"s", b"S"):
            _clear()
            print("==> 正在运行自检 ...\n")
            output = _capture(
                [
                    sys.executable,
                    str(install_dir / "bitweb_monitor.py"),
                    "--config",
                    str(config_path),
                    "--self-test",
                ]
            )
            print(output)
            _pause()
            continue
        if key in (b"p", b"P"):
            _clear()
            print("==> 设置密码（输入不会回显）\n")
            _run_interactive(
                [
                    sys.executable,
                    str(install_dir / "set_password.py"),
                    "--config",
                    str(config_path),
                ]
            )
            _pause()
            continue
        if key in (b"l", b"L"):
            log_path = Path(data.get("log_path", ""))
            if log_path.exists():
                subprocess.Popen(["notepad.exe", str(log_path)])
            continue
        if key in (b"g", b"G"):
            gui_ps1 = install_dir / "gui.ps1"
            if gui_ps1.exists():
                subprocess.Popen(
                    [
                        _powershell(),
                        "-NoProfile",
                        "-ExecutionPolicy",
                        "Bypass",
                        "-File",
                        str(gui_ps1),
                        "-InstallDir",
                        str(install_dir),
                    ]
                )
            continue
        if key in (b"d", b"D"):
            _clear()
            print("==> 重启守护进程 ...")
            _run_schtasks(["/end", "/tn", TASK_NAME])
            _start_daemon(install_dir, config_path)
            _pause()
            continue
        if key in (b"o", b"O"):
            subprocess.Popen(["explorer.exe", str(install_dir)])
            continue


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
