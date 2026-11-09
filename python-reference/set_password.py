#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""设置 / 更新 BIT-Web 门户密码（DPAPI 加密后写入 config.json）。

用法（在装有本工具的目录下）：
    python set_password.py
    python set_password.py --config "C:\\...\\config.json"
    python set_password.py --username 2024000001   # 顺便更新学号

密码只在内存中出现，使用 Windows DPAPI（当前用户作用域）加密后以
Base64 形式存储；换 Windows 用户或换机器后无法解密，需要重新运行本脚本。
"""

from __future__ import annotations

import argparse
import getpass
import sys
from pathlib import Path

import appconfig
import dpapi


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="设置 BIT-Web 自动登录密码")
    parser.add_argument("--config", help="config.json 路径（默认自动定位）")
    parser.add_argument("--username", help="同时更新用户名 / 学号")
    args = parser.parse_args(argv)

    config_path = appconfig.resolve_config_path(args.config)
    appconfig.ensure_config(config_path)
    config = appconfig.load_config(config_path)

    if args.username:
        config["username"] = args.username.strip()

    if config.get("username"):
        print("用户名 / 学号: %s" % config["username"])
        print("（如需修改，重新运行并加 --username 学号）")
    else:
        entered = input("请输入用户名 / 学号: ").strip()
        if not entered:
            print("错误：用户名不能为空", file=sys.stderr)
            return 2
        config["username"] = entered

    print()
    print("请输入校园网密码（输入时不会回显，也不会保存明文）：")
    try:
        password = getpass.getpass("密码: ")
        again = getpass.getpass("再输入一次确认: ")
    except (KeyboardInterrupt, EOFError):
        print("\n已取消", file=sys.stderr)
        return 1

    if not password:
        print("错误：密码不能为空", file=sys.stderr)
        return 2
    if password != again:
        print("错误：两次输入不一致", file=sys.stderr)
        return 2

    try:
        encrypted = dpapi.protect(password.encode("utf-8"))
    except Exception as exc:  # noqa: BLE001
        print("DPAPI 加密失败：%s" % exc, file=sys.stderr)
        return 1

    config["password_dpapi"] = encrypted
    config["password"] = ""  # 清除可能存在的明文
    appconfig.save_config(config_path, config)

    print()
    print("已加密保存到: %s" % config_path)
    print("守护进程下一次尝试登录时会自动读取新密码，无需重启。")
    print("如需立即验证，可运行: python bitweb_monitor.py --self-test")
    return 0


if __name__ == "__main__":
    sys.exit(main())
