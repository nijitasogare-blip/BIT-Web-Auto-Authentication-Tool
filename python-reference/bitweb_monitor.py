#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BIT-Web 自动登录守护进程。

行为约定（与需求一一对应）：
  * 开机登录 Windows 后由计划任务自动启动（见 install.ps1）。
  * 每 15 秒读取当前 WLAN SSID：
      - 只有 SSID 精确等于白名单（默认「BIT-Web」）时才工作；
      - 其他无线网 / 有线网 / 未连接：进入休眠，不访问门户、不发送任何个人信息。
  * 一旦检测到白名单 SSID：立即检查外网连通性，不通则自动登录 10.0.0.55。
  * 在保持 BIT-Web 连接期间：每 5 分钟 ping 外网确认一次，断网则自动重新登录。
  * 认证失败（如密码错误）会冷却 30 分钟，避免反复失败触发账号风控。

手动用法：
  pythonw.exe bitweb_monitor.py                       # 后台常驻（计划任务用）
  python  bitweb_monitor.py --self-test               # 安装后自检（推荐）
  python  bitweb_monitor.py --once                    # 只跑一轮，便于调试
  python  bitweb_monitor.py --once --dry-run          # 只报告，不真正发登录请求
  python  bitweb_monitor.py --simulate-offline --dry-run --once   # 演练断网流程
"""

from __future__ import annotations

import argparse
import ctypes
import logging
import logging.handlers
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import appconfig
import dpapi
import srun_client

_CREATE_NO_WINDOW = 0x08000000
_MUTEX_NAME = "Local\\BITWebAutoLogin.Singleton"

_NAME_RE = re.compile(r"^[ \t]*(?:Name|名称)[ \t]*:[ \t]*(.*)$", re.I)
_SSID_RE = re.compile(r"^[ \t]*SSID[ \t]*:[ \t]*(.*)$", re.I)
_STATE_RE = re.compile(r"^[ \t]*(?:State|状态)[ \t]*:[ \t]*(.*)$", re.I)
_LATENCY_RE = re.compile(r"(?:time|时间)[=<][ \t]*(\d+)[ \t]*ms", re.I)
_AUTH_KEYWORDS = (
    "密码", "password", "用户名", "账号", "账户", "不存在",
    "冻结", "锁定", "验证码", "欠费", "余额", "过期", "停机",
    "invalid", "not found", "login failed",
)


# ---------------------------------------------------------------------------
# 日志
# ---------------------------------------------------------------------------

def _build_logger(config: dict, config_path: Path, verbose: bool) -> logging.Logger:
    logger = logging.getLogger("bitweb")
    logger.setLevel(logging.DEBUG)
    logger.handlers.clear()
    logger.propagate = False

    level_name = str(config.get("logging", {}).get("level", "INFO")).upper()
    level = getattr(logging, level_name, logging.INFO)
    fmt = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s", "%Y-%m-%d %H:%M:%S")

    log_file = appconfig.resolve_relative(config_path, config["logging"]["file"])
    log_file.parent.mkdir(parents=True, exist_ok=True)
    file_handler = logging.handlers.RotatingFileHandler(
        str(log_file),
        maxBytes=int(config["logging"].get("max_bytes", 1048576)),
        backupCount=int(config["logging"].get("backup_count", 3)),
        encoding="utf-8",
    )
    file_handler.setLevel(level)
    file_handler.setFormatter(fmt)
    logger.addHandler(file_handler)

    # pythonw.exe 下 sys.stderr 为 None，不能加控制台 handler
    stderr = getattr(sys, "stderr", None)
    if stderr is not None and (verbose or getattr(stderr, "isatty", lambda: False)()):
        console = logging.StreamHandler()
        console.setLevel(logging.DEBUG if verbose else level)
        console.setFormatter(fmt)
        logger.addHandler(console)

    logger.debug("日志文件: %s", log_file)
    return logger


# ---------------------------------------------------------------------------
# WLAN / 连通性检测
# ---------------------------------------------------------------------------

def _decode_console(data: bytes) -> str:
    for encoding in ("mbcs", "utf-8", "gbk"):
        try:
            return data.decode(encoding)
        except (UnicodeDecodeError, LookupError):
            continue
    return data.decode("utf-8", errors="replace")


def _split_blocks(text: str) -> list[list[str]]:
    blocks: list[list[str]] = []
    current: list[str] | None = None
    for line in text.splitlines():
        if _NAME_RE.match(line):
            if current:
                blocks.append(current)
            current = [line]
        elif current is not None:
            current.append(line)
    if current:
        blocks.append(current)
    return blocks


def parse_wlan_interfaces(text: str) -> list[dict]:
    """解析 `netsh wlan show interfaces` 输出（兼容中/英文系统）。"""
    interfaces = []
    for block in _split_blocks(text):
        name = ""
        ssid = ""
        state = ""
        for line in block:
            match = _NAME_RE.match(line)
            if match and not name:
                name = match.group(1).strip()
                continue
            match = _SSID_RE.match(line)
            if match:
                ssid = match.group(1).strip()
                continue
            match = _STATE_RE.match(line)
            if match:
                state = match.group(1).strip()
        state_lower = state.lower()
        connected = (
            ("connected" in state_lower and "disconnected" not in state_lower)
            or "已连接" in state
        )
        interfaces.append(
            {"name": name, "ssid": ssid, "state": state, "connected": connected}
        )
    return interfaces


def get_wlan_interfaces() -> tuple[list[dict], str]:
    try:
        proc = subprocess.run(
            ["netsh", "wlan", "show", "interfaces"],
            capture_output=True,
            timeout=10,
            creationflags=_CREATE_NO_WINDOW,
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return [], "调用 netsh 失败: %s" % exc
    text = _decode_console(proc.stdout or b"") + _decode_console(proc.stderr or b"")
    if proc.returncode != 0 and not text.strip():
        return [], "netsh 返回码 %s" % proc.returncode
    return parse_wlan_interfaces(text), ""


def ssid_allowed(ssid: str, allowed: list[str]) -> bool:
    """精确匹配（大小写敏感），避免连到仿冒 SSID 时泄露账号。"""
    return any(ssid == item for item in allowed if item)


def ping_once(target: str, timeout_ms: int) -> tuple[bool, str]:
    try:
        proc = subprocess.run(
            ["ping", "-n", "1", "-w", str(int(timeout_ms)), target],
            capture_output=True,
            timeout=timeout_ms / 1000.0 + 3,
            creationflags=_CREATE_NO_WINDOW,
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return False, "ping 执行失败: %s" % exc
    output = _decode_console(proc.stdout or b"") + _decode_console(proc.stderr or b"")
    if "TTL=" not in output.upper():
        return False, "ping %s 无响应" % target
    match = _LATENCY_RE.search(output)
    latency = "%sms" % match.group(1) if match else "?"
    return True, "ping %s 正常（%s）" % (target, latency)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _is_portal_text(text: str, portal_host: str) -> bool:
    lowered = text.lower()
    if portal_host and portal_host in text:
        return True
    return any(
        marker in lowered
        for marker in ("srun_portal", "eportal", "深澜", "网络准入认证")
    )


def _http_probe(url: str, timeout: float, portal_host: str) -> tuple[bool, str]:
    """返回是否可达外网；若被重定向/返回校园认证页则视为离线。"""
    try:
        opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), _NoRedirect()
        )
        req = urllib.request.Request(
            url, headers={"User-Agent": srun_client.USER_AGENT, "Accept": "*/*"}
        )
        with opener.open(req, timeout=timeout) as resp:
            body = resp.read(2048)
            if resp.status == 200 and body:
                text = body.decode("utf-8", errors="replace")
                if _is_portal_text(text, portal_host):
                    return False, "HTTP 200 但返回的是认证门户页面"
                return True, "HTTP %s" % resp.status
            return False, "HTTP %s 内容异常" % resp.status
    except urllib.error.HTTPError as exc:
        location = exc.headers.get("Location", "") or ""
        if exc.code in (301, 302, 303, 307, 308):
            if _is_portal_text(location, portal_host):
                return False, "被重定向到认证门户（HTTP %s）" % exc.code
            return True, "HTTP %s 跳转到外网" % exc.code
        return False, "HTTP %s" % exc.code
    except (urllib.error.URLError, OSError, ValueError) as exc:
        return False, "HTTP 探测失败: %s" % exc


def check_connectivity(config: dict) -> tuple[bool, str]:
    """判断外网是否可达。

    优先用 HTTP 探测（快、且能识别"被劫持到认证门户"），失败再用 ping；
    任一成功即在线。校园网经常屏蔽 ICMP，HTTP 兜底是主路径。
    """
    conn = config["connectivity"]
    last_detail = ""
    if conn.get("http_probe", True):
        ok, detail = _http_probe(
            str(conn["http_probe_url"]),
            float(conn.get("http_probe_timeout_sec", 5)),
            str(config["portal"]["host"]),
        )
        if ok:
            return True, detail
        last_detail = detail
    timeout_ms = int(conn["ping_timeout_ms"])
    for target in conn["ping_targets"]:
        ok, detail = ping_once(str(target), timeout_ms)
        if ok:
            return True, detail
        last_detail = detail
    return False, last_detail or "连通性检测失败"


# ---------------------------------------------------------------------------
# 登录
# ---------------------------------------------------------------------------

def read_password(config: dict) -> str:
    token = str(config.get("password_dpapi") or "").strip()
    if token:
        return dpapi.unprotect(token).decode("utf-8")
    return str(config.get("password") or "")


def _is_auth_error(message: str) -> bool:
    lowered = message.lower()
    return any(keyword in lowered for keyword in _AUTH_KEYWORDS)


def attempt_login(config: dict, logger: logging.Logger, dry_run: bool = False) -> str:
    """执行一次登录。

    返回 ok / already_online / no_password / auth_error / failed / dry_run。
    already_online 表示门户提示"该 IP 已在线"（error 仍为 ok），这不算
    真正重新认证成功，调用方需要区分处理。
    """
    portal = config["portal"]
    host = str(portal["host"])
    scheme = str(portal.get("scheme") or "http")
    timeout = float(portal.get("timeout_sec") or 6)
    username = str(config.get("username") or "").strip()

    if dry_run:
        detail = ""
        try:
            discovered = srun_client.discover_portal(host, scheme, timeout)
            detail = "门户 ac_id=%s，本机 IP=%s" % (
                discovered.get("ac_id") or "?",
                discovered.get("ip") or "?",
            )
        except Exception as exc:  # noqa: BLE001 - 演练模式不因探测失败退出
            detail = "门户参数探测失败：%s" % exc
        logger.info("dry-run：检测到断网，本应提交认证请求（已跳过）；%s", detail)
        return "dry_run"

    if not username:
        logger.error("配置文件里未填写用户名（学号），无法登录")
        return "no_password"
    try:
        password = read_password(config)
    except Exception as exc:  # DPAPI 解密失败（例如换了 Windows 用户）
        logger.error("密码解密失败（可能不是同一 Windows 用户）：%s", exc)
        return "no_password"
    if not password:
        logger.error("尚未设置密码，请先运行 set_password.py")
        return "no_password"
    if not username.isascii() or not password.isascii():
        logger.error("学号/密码包含非 ASCII 字符，当前协议实现不支持，请改成 ASCII 字符")
        return "auth_error"

    try:
        discovered = srun_client.discover_portal(host, scheme, timeout)
        ac_id = str(portal.get("ac_id") or discovered.get("ac_id") or "").strip()
        if not ac_id:
            logger.error("无法从门户页面识别 ac_id，请检查是否已连上 BIT-Web")
            return "failed"
        ip = str(discovered.get("ip") or "").strip()
        logger.debug("门户参数: ac_id=%s ip=%s", ac_id, ip)
        response = srun_client.portal_login(
            host=host,
            username=username,
            password=password,
            ac_id=ac_id,
            ip=ip,
            scheme=scheme,
            timeout=timeout,
            ignore=int(config["login"].get("ignore", 2)),
        )
        if srun_client.is_already_online(response):
            logger.warning(
                "门户返回“该 IP 已在线”（ip=%s），说明 AC 上仍有残留会话，未真正重新认证",
                ip or response.get("online_ip", "?"),
            )
            return "already_online"
        logger.info(
            "认证接口返回成功（user=%s, ip=%s）",
            appconfig.mask_user(username), ip or response.get("client_ip", "?"),
        )
        return "ok"
    except srun_client.PortalError as exc:
        message = str(exc)
        logger.warning("登录失败: %s", message)
        if "already" in message.lower() or "已在线" in message:
            logger.warning("门户提示该 IP 已在线，转入残留会话处理")
            return "already_online"
        if _is_auth_error(message):
            return "auth_error"
        return "failed"
    except srun_client.SrunError as exc:
        logger.warning("登录请求异常: %s", exc)
        return "failed"
    except Exception as exc:  # noqa: BLE001 - 守护进程必须存活
        logger.exception("登录出现未预期异常: %s", exc)
        return "failed"


# ---------------------------------------------------------------------------
# 主循环
# ---------------------------------------------------------------------------

class Monitor:
    def __init__(
        self,
        config: dict,
        config_path: Path,
        logger: logging.Logger,
        dry_run: bool = False,
        simulate_ssid: str | None = None,
        simulate_offline: bool = False,
    ):
        self.config = config
        self.config_path = config_path
        self.logger = logger
        self.dry_run = dry_run
        self.simulate_ssid = simulate_ssid
        self.simulate_offline = simulate_offline

        self.mode = "sleep"            # sleep / active
        self.sleep_pending = 0
        self.next_check = 0.0
        self.auth_fail_until = 0.0
        self.config_error_until = 0.0
        try:
            self._config_mtime = self.config_path.stat().st_mtime
        except OSError:
            self._config_mtime = None

    def _maybe_reload_config(self) -> None:
        """配置文件被修改（如 set_password.py 写入新密码）时热加载。

        只有用户名 / 密码变化时才重置登录冷却；改其他配置不影响冷却。
        """
        try:
            mtime = self.config_path.stat().st_mtime
        except OSError:
            return
        if mtime == self._config_mtime:
            return
        self._config_mtime = mtime
        old_user = str(self.config.get("username") or "")
        old_secret = "%s|%s" % (
            self.config.get("password_dpapi") or "",
            self.config.get("password") or "",
        )
        try:
            new_config = appconfig.load_config(self.config_path)
        except Exception as exc:  # noqa: BLE001
            self.logger.warning("配置文件已改动但读取失败，继续使用旧配置：%s", exc)
            return
        new_user = str(new_config.get("username") or "")
        new_secret = "%s|%s" % (
            new_config.get("password_dpapi") or "",
            new_config.get("password") or "",
        )
        self.config = new_config
        if new_user != old_user or new_secret != old_secret:
            self.auth_fail_until = 0.0
            self.config_error_until = 0.0
            self.logger.info("检测到用户名/密码更新，已重新加载并重置登录冷却")
        else:
            self.logger.info("检测到配置文件更新，已重新加载")

    # ---- 网络状态 -----------------------------------------------------------
    def _current_wlan(self) -> tuple[list[dict], str]:
        if self.simulate_ssid is not None:
            fake = [
                {
                    "name": "WLAN(模拟)",
                    "ssid": self.simulate_ssid,
                    "state": "已连接",
                    "connected": True,
                }
            ]
            return fake, ""
        return get_wlan_interfaces()

    def _active_ssid(self, interfaces: list[dict]) -> str | None:
        allowed = [str(s) for s in self.config["network"]["allowed_ssids"]]
        for item in interfaces:
            if item.get("connected") and item.get("ssid"):
                if ssid_allowed(item["ssid"], allowed):
                    return item["ssid"]
        return None

    def _connected_ssids(self, interfaces: list[dict]) -> list[str]:
        return [i["ssid"] for i in interfaces if i.get("connected") and i.get("ssid")]

    # ---- 一轮检测 -----------------------------------------------------------
    def tick(self, once: bool = False) -> None:
        self._maybe_reload_config()
        interfaces, wlan_error = self._current_wlan()
        if wlan_error:
            self.logger.debug("WLAN 状态读取失败: %s", wlan_error)

        active_ssid = self._active_ssid(interfaces)
        now = time.monotonic()

        if active_ssid:
            self.sleep_pending = 0
            if self.mode != "active":
                self.mode = "active"
                self.auth_fail_until = 0.0
                self.config_error_until = 0.0
                self.next_check = now
                self.logger.info(
                    "检测到白名单 SSID「%s」，开始监听（下次连通性检查：立即）",
                    active_ssid,
                )
        else:
            if self.mode == "active":
                self.sleep_pending += 1
                confirm = int(self.config["network"].get("sleep_confirm_polls", 2))
                if self.sleep_pending >= confirm:
                    self.mode = "sleep"
                    self.next_check = 0.0
                    current = self._connected_ssids(interfaces)
                    self.logger.info(
                        "当前无白名单 SSID（已连接：%s），进入休眠：不访问门户、不发送任何信息",
                        "、".join(current) if current else "无",
                    )
            if self.mode == "sleep":
                if once:
                    self.logger.info("一次性检查完成：当前不监听（休眠）")
                return

        if self.sleep_pending:
            # 白名单 SSID 刚消失，等待确认，不做任何网络请求
            return

        if now < self.next_check:
            return

        self._run_connectivity_cycle(once=once)

    def _run_connectivity_cycle(self, once: bool) -> None:
        interval = int(self.config["connectivity"]["check_interval_sec"])
        if self.simulate_offline:
            online, detail = False, "模拟断网（--simulate-offline）"
        else:
            online, detail = check_connectivity(self.config)

        if online:
            self.logger.info("连通性正常：%s", detail)
            self.auth_fail_until = 0.0
            self.config_error_until = 0.0
            self.next_check = time.monotonic() + interval
            return

        self.logger.warning("外网不通：%s", detail)
        now = time.monotonic()
        if now < self.auth_fail_until:
            remaining = int(self.auth_fail_until - now)
            self.logger.warning(
                "登录冷却中（还剩 %d 秒），本轮不重试；如密码已修改可运行 set_password.py",
                remaining,
            )
            self.next_check = now + interval
            return
        if now < self.config_error_until:
            remaining = int(self.config_error_until - now)
            self.logger.warning(
                "未配置密码/配置异常冷却中（还剩 %d 秒）；请运行 set_password.py 设置密码",
                remaining,
            )
            self.next_check = now + interval
            return

        result = self._login_flow()
        self.next_check = time.monotonic() + interval
        if result:
            self.logger.info("网络已恢复，%d 秒后再次检查", interval)
        elif self.mode == "active":
            self.logger.warning("%d 秒后再次检查并尝试恢复", interval)

    def _wait_online(self) -> bool:
        """认证接口返回后，用外网连通性确认是否真的登录成功。"""
        for wait in (1.5, 3.0):
            time.sleep(wait)
            online, detail = check_connectivity(self.config)
            if online:
                self.logger.info("登录成功，外网恢复：%s", detail)
                return True
        self.logger.warning("认证接口返回成功，但外网仍不通")
        return False

    def _logout_residual_session(self) -> bool:
        """清理 AC 上残留的在线会话（登录返回 ip_already_online 时使用）。"""
        portal = self.config["portal"]
        host = str(portal["host"])
        scheme = str(portal.get("scheme") or "http")
        timeout = float(portal.get("timeout_sec") or 6)
        try:
            discovered = srun_client.discover_portal(host, scheme, timeout)
            ac_id = str(portal.get("ac_id") or discovered.get("ac_id") or "").strip()
            if not ac_id:
                self.logger.warning("无法确定 ac_id，跳过注销残留会话")
                return False
            srun_client.portal_logout(
                host=host,
                ac_id=ac_id,
                ip=str(discovered.get("ip") or ""),
                scheme=scheme,
                timeout=timeout,
            )
            self.logger.info("已注销残留会话（ip=%s），稍后重新认证", discovered.get("ip", "?"))
            return True
        except srun_client.SrunError as exc:
            self.logger.warning("注销残留会话失败：%s", exc)
            return False

    def _login_flow(self) -> bool:
        login_cfg = self.config["login"]
        retries = max(0, int(login_cfg.get("retry_count", 2)))
        delay = float(login_cfg.get("retry_delay_sec", 5))
        backoff = int(login_cfg.get("auth_error_backoff_sec", 1800))

        attempts = retries + 1
        for attempt in range(1, attempts + 1):
            self.logger.info("尝试自动登录（第 %d/%d 次）", attempt, attempts)
            result = attempt_login(self.config, self.logger, self.dry_run)

            if result == "ok":
                if self._wait_online():
                    return True
                # 返回成功但外网仍不通：继续重试
            elif result == "already_online":
                # 门户认为 IP 仍在线，但外网不通：会话残留，先注销再认证一次
                self.logger.warning("门户提示 IP 已在线但外网不通，疑似残留会话")
                if self._logout_residual_session():
                    retry_result = attempt_login(self.config, self.logger, self.dry_run)
                    if retry_result == "ok" and self._wait_online():
                        return True
                    if retry_result == "auth_error":
                        self._set_auth_cooldown(backoff)
                        return False
                self.logger.error(
                    "残留会话清理后仍无法恢复外网：10 分钟内不再重试。"
                    "可手动打开 http://10.0.0.55 退出登录后再试"
                )
                self.auth_fail_until = time.monotonic() + 600
                return False
            elif result == "dry_run":
                return False
            elif result == "no_password":
                self.config_error_until = time.monotonic() + 600
                self.logger.warning(
                    "未配置密码或密码不可用：10 分钟内不再重复尝试；请运行 set_password.py"
                )
                return False
            elif result == "auth_error":
                self._set_auth_cooldown(backoff)
                return False

            if attempt < attempts:
                time.sleep(delay)
        return False

    def _set_auth_cooldown(self, backoff: int) -> None:
        self.auth_fail_until = time.monotonic() + backoff
        self.logger.error(
            "疑似账号/密码问题，%d 分钟内不再自动重试；"
            "请核对学号密码，或运行 set_password.py 更新",
            backoff // 60,
        )


def acquire_single_instance() -> object | None:
    """命名互斥量，防止计划任务/手动调试同时跑两个实例。"""
    if sys.platform != "win32":
        return object()
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateMutexW.restype = ctypes.c_void_p
    handle = kernel32.CreateMutexW(None, False, _MUTEX_NAME)
    if not handle:
        return object()
    if kernel32.GetLastError() == 183:  # ERROR_ALREADY_EXISTS
        kernel32.CloseHandle(handle)
        return None
    return handle


# ---------------------------------------------------------------------------
# 自检
# ---------------------------------------------------------------------------

def self_test(config_path: Path, config: dict) -> int:
    print("=" * 64)
    print("BIT-Web 自动登录 - 安装自检")
    print("=" * 64)
    print("配置文件 : %s" % config_path)
    print("用户名   : %s" % appconfig.mask_user(str(config.get("username") or "")))

    password_ok = False
    password_detail = ""
    try:
        password_ok = bool(read_password(config))
    except Exception as exc:  # noqa: BLE001
        password_detail = "解密失败：%s" % exc
    if password_ok:
        print("密码     : 已配置（DPAPI 加密）")
    else:
        print("密码     : 未配置%s（运行 python set_password.py）"
              % ("，" + password_detail if password_detail else ""))

    allowed = [str(s) for s in config["network"]["allowed_ssids"]]
    print("白名单   : %s" % "、".join(allowed))

    interfaces, wlan_error = get_wlan_interfaces()
    if wlan_error:
        print("WLAN     : 读取失败（%s）" % wlan_error)
    elif not interfaces:
        print("WLAN     : 未发现无线网卡")
    else:
        for item in interfaces:
            print(
                "WLAN     : %s -> %s（%s）"
                % (item["name"] or "?", item["ssid"] or "未连接", item["state"] or "?")
            )

    active_ssid = None
    for item in interfaces:
        if item.get("connected") and ssid_allowed(item.get("ssid", ""), allowed):
            active_ssid = item["ssid"]
            break

    if not active_ssid:
        print("监听状态 : 休眠（当前不是白名单 SSID，已按隐私策略跳过门户探测）")
        print()
        print("结论：配置可被读取；连上 BIT-Web 后守护进程会自动开始工作。")
        return 0

    print("监听状态 : 工作中的 SSID「%s」" % active_ssid)
    online, detail = check_connectivity(config)
    print("外网连通 : %s（%s）" % ("正常" if online else "不通", detail))

    portal = config["portal"]
    host = str(portal["host"])
    scheme = str(portal.get("scheme") or "http")
    timeout = float(portal.get("timeout_sec") or 6)
    try:
        discovered = srun_client.discover_portal(host, scheme, timeout)
        print("认证门户 : %s" % discovered["url"])
        print("ac_id    : %s    本机 IP: %s"
              % (discovered.get("ac_id") or "(自动)", discovered.get("ip") or "?"))
    except Exception as exc:  # noqa: BLE001
        print("认证门户 : 不可达（%s）" % exc)
        return 1

    username = str(config.get("username") or "").strip()
    if username:
        try:
            challenge = srun_client.get_challenge(host, username, discovered.get("ip", ""), scheme, timeout)
            print("challenge: 正常（token 长度 %d）" % len(challenge.get("challenge", "")))
        except Exception as exc:  # noqa: BLE001
            print("challenge: 失败（%s）" % exc)
            return 1
    else:
        print("challenge: 跳过（未填写用户名）")

    print()
    if online:
        print("结论：当前已在线，配置正常。守护进程会每 5 分钟自动检查，断线后自动重登。")
    else:
        print("结论：当前外网不通，配置正常。守护进程会自动尝试登录。")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="BIT-Web（10.0.0.55）断线自动登录守护进程"
    )
    parser.add_argument("--config", help="指定 config.json 路径")
    parser.add_argument("--once", action="store_true", help="只运行一轮后退出（调试用）")
    parser.add_argument("--self-test", action="store_true", help="安装后自检，不发起登录")
    parser.add_argument("--dry-run", action="store_true", help="只检测，不真正发送登录请求")
    parser.add_argument("--simulate-ssid", help="模拟当前 SSID（测试白名单/休眠逻辑）")
    parser.add_argument("--simulate-offline", action="store_true", help="模拟断网（测试登录流程）")
    parser.add_argument("--verbose", "-v", action="store_true", help="输出 DEBUG 日志")
    parser.add_argument("--no-single-instance", action="store_true", help=argparse.SUPPRESS)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = build_args(argv)
    config_path = appconfig.resolve_config_path(args.config)
    created = appconfig.ensure_config(config_path)
    config = appconfig.load_config(config_path)
    logger = _build_logger(config, config_path, args.verbose)
    if created:
        logger.warning("已生成默认配置文件：%s", config_path)

    if args.self_test:
        return self_test(config_path, config)

    lock = object()
    if not args.no_single_instance:
        lock = acquire_single_instance()
        if lock is None:
            if args.once:
                logger.warning(
                    "检测到守护进程正在运行：--once 自动改为只读模式（不执行登录），"
                    "避免与守护进程重复认证"
                )
                args.dry_run = True
                lock = object()
            else:
                logger.warning("已有实例在运行，本实例退出")
                return 0

    monitor = Monitor(
        config=config,
        config_path=config_path,
        logger=logger,
        dry_run=args.dry_run,
        simulate_ssid=args.simulate_ssid,
        simulate_offline=args.simulate_offline,
    )

    if args.once:
        monitor.tick(once=True)
        return 0

    logger.info("守护进程启动（pid=%s，配置=%s）", _pid(), config_path)
    poll = max(5, int(config["network"].get("ssid_poll_sec", 15)))
    try:
        while True:
            try:
                monitor.tick()
            except Exception as exc:  # noqa: BLE001 - 守护进程必须存活
                logger.exception("本轮检测出现异常: %s", exc)
                time.sleep(30)
            time.sleep(poll)
    except KeyboardInterrupt:
        logger.info("收到退出信号，守护进程结束")
    finally:
        del lock
    return 0


def _pid() -> int:
    try:
        return int(ctypes.windll.kernel32.GetCurrentProcessId())
    except Exception:  # noqa: BLE001
        return -1


def _write_crash_log(exc: BaseException) -> None:
    """pythonw 下没有控制台，异常写盘以便排查。"""
    import traceback

    try:
        crash_dir = Path(__file__).resolve().parent / "logs"
        crash_dir.mkdir(parents=True, exist_ok=True)
        with (crash_dir / "crash.log").open("a", encoding="utf-8") as fh:
            fh.write("\n[%s] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), exc))
            fh.write(traceback.format_exc())
            fh.write("\n")
    except Exception:  # noqa: BLE001
        pass
    stderr = getattr(sys, "stderr", None)
    if stderr is not None:
        try:
            traceback.print_exc()
        except Exception:  # noqa: BLE001
            pass


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except BaseException as exc:  # noqa: BLE001 - 兜底写盘，避免 pythonw 静默退出
        _write_crash_log(exc)
        sys.exit(1)
