#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""配置加载 / 保存 / 路径解析。"""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path

APP_NAME = "BITWebAutoLogin"
APP_VERSION = "1.0.0"
SCRIPT_DIR = Path(__file__).resolve().parent
LOCAL_APPDATA = Path(os.environ.get("LOCALAPPDATA") or Path.home())
INSTALL_DIR = LOCAL_APPDATA / APP_NAME
INSTALL_CONFIG = INSTALL_DIR / "config.json"

DEFAULT_CONFIG = {
    "username": "",
    "password_dpapi": "",
    # 兼容用：不建议手写明文密码，留空即可（由 set_password.py 写入上面字段）
    "password": "",
    "portal": {
        "host": "10.0.0.55",
        "scheme": "http",
        "ac_id": "",          # 留空则自动从门户页面识别（当前为 43）
        "timeout_sec": 6,
    },
    "network": {
        # 只有连到这些 SSID 才工作；大小写敏感、精确匹配
        "allowed_ssids": ["BIT-Web"],
        "ssid_poll_sec": 15,
        # 连续多少次检测不到白名单 SSID 才真正休眠（防止瞬断误判）
        "sleep_confirm_polls": 2,
    },
    "connectivity": {
        "check_interval_sec": 300,   # 每 5 分钟确认一次外网连通性
        "ping_targets": ["baidu.com", "114.114.114.114"],
        "ping_timeout_ms": 2000,
        "http_probe": True,
        "http_probe_url": "http://www.baidu.com/",
        "http_probe_timeout_sec": 5,
    },
    "login": {
        "retry_count": 2,
        "retry_delay_sec": 5,
        # 密码错误 / 账号异常时，冷却这么久再尝试，避免频繁失败触发风控
        "auth_error_backoff_sec": 1800,
        "ignore": 2,
    },
    "logging": {
        "level": "INFO",
        "file": "logs/bitweb_monitor.log",
        "max_bytes": 1048576,
        "backup_count": 3,
    },
}


def _deep_merge(base: dict, override: dict) -> dict:
    result = copy.deepcopy(base)
    for key, value in (override or {}).items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = _deep_merge(result[key], value)
        else:
            result[key] = value
    return result


def resolve_config_path(cli_value: str | None = None) -> Path:
    """优先级：命令行 > 环境变量 BITWEB_CONFIG > 安装目录 > 脚本目录。"""
    if cli_value:
        return Path(cli_value).expanduser().resolve()
    env_value = os.environ.get("BITWEB_CONFIG")
    if env_value:
        return Path(env_value).expanduser().resolve()
    if INSTALL_CONFIG.exists():
        return INSTALL_CONFIG.resolve()
    return (SCRIPT_DIR / "config.json").resolve()


def load_config(path: Path) -> dict:
    if not path.exists():
        return copy.deepcopy(DEFAULT_CONFIG)
    with path.open("r", encoding="utf-8-sig") as fh:
        raw = json.load(fh)
    if not isinstance(raw, dict):
        raise ValueError("config.json 顶层必须是 JSON 对象")
    return _deep_merge(DEFAULT_CONFIG, raw)


def save_config(path: Path, config: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with tmp.open("w", encoding="utf-8", newline="\n") as fh:
        json.dump(config, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    os.replace(tmp, path)


def ensure_config(path: Path) -> bool:
    """配置文件不存在时生成默认模板；返回是否新建。"""
    if path.exists():
        return False
    save_config(path, DEFAULT_CONFIG)
    return True


def mask_user(name: str) -> str:
    name = (name or "").strip()
    if not name:
        return "<未填写>"
    if len(name) <= 4:
        return "*" * len(name)
    return name[:4] + "*" * (len(name) - 4)


def resolve_relative(base_file: Path, value: str) -> Path:
    p = Path(value)
    return p if p.is_absolute() else (base_file.parent / p)
