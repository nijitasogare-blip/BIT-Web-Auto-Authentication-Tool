#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BIT-Web（深澜 / Srun 门户 10.0.0.55）认证客户端。

只依赖 Python 标准库（Windows 自带 Python 3.8+ 即可运行）。

协议依据 2026-11 从门户实际下发的前端脚本还原：
  /static/js/jquery.srun.portal.js?v=2.00.20220802  ($.Login)
  /static/js/main.js?v=2.00.20220802                (登录参数构造)
  /static/bower/all/1.0.0/all.min.js                (md5 / sha1 / base64)

流程：
  1. GET  /cgi-bin/get_challenge           -> challenge / token
  2. info  = "{SRBX1}" + base64_custom(xEncode(JSON(info), token))
     hmd5  = HMAC-MD5(key=token, msg=password)      (js-md5 两参数形式)
  3. chksum = SHA1(token+user+token+hmd5+token+ac_id+token+ip+token+200+token+1+token+info)
  4. GET  /cgi-bin/srun_portal?action=login...    -> {"error":"ok"} 表示成功

安全说明：本模块只在调用方确认当前 SSID 属于白名单后才会被调用；
密码以明文只存在于内存，不写日志。
"""

from __future__ import annotations

import base64 as _b64
import hashlib
import hmac
import json
import re
import socket
import time
import urllib.error
import urllib.parse
import urllib.request

__all__ = [
    "SrunError",
    "PortalError",
    "xencode",
    "srun_base64",
    "hmac_md5_hex",
    "make_info",
    "build_login_params",
    "get_challenge",
    "portal_login",
    "portal_logout",
    "is_already_online",
    "get_online_info",
    "discover_portal",
    "local_ip_towards",
    "parse_jsonp",
]

# 门户前端 $.base64 使用的自定义字母表（Srun 私有变体）。
CUSTOM_ALPHA = "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA"
STD_ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
_ALPHA_MAP = str.maketrans(STD_ALPHA, CUSTOM_ALPHA)

ENC_VER = "srun_bx1"
N = 200
_MSG_TYPE = 1
USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)
_UA = USER_AGENT
_JSONP_RE = re.compile(r"^\s*[\w$.]+\s*\((.*)\)\s*;?\s*$", re.S)


class SrunError(Exception):
    """协议/网络层错误。"""


# 门户返回业务错误（例如密码错误）时抛出；status 为门户原始返回。
class PortalError(SrunError):
    def __init__(self, message: str, payload: dict | None = None):
        super().__init__(message)
        self.payload = payload or {}


# ---------------------------------------------------------------------------
# 纯算法部分（与门户 JS 逐位等价）
# ---------------------------------------------------------------------------

def _to_words(text: str, include_length: bool) -> list[int]:
    """把字符串按 UTF-8 字节每 4 个打包成 32 位整数（对应 JS 的 s()）。

    门户 JS 直接对 UTF-16 code unit 打包；账号/密码/JSON 均为 ASCII 时
    两者完全一致。这里使用 UTF-8 字节，与主流 Srun 实现保持一致。
    """
    data = text.encode("utf-8")
    words = []
    for i in range(0, len(data), 4):
        word = 0
        for j in range(4):
            if i + j < len(data):
                word |= data[i + j] << (8 * j)
        words.append(word & 0xFFFFFFFF)
    if include_length:
        words.append(len(data) & 0xFFFFFFFF)
    return words


def _from_words(words: list[int]) -> str:
    """对应 JS 的 l()：每个 32 位整数按小端拆成 4 个字符（0-255）。"""
    chars = []
    for word in words:
        chars.append(chr(word & 0xFF))
        chars.append(chr((word >> 8) & 0xFF))
        chars.append(chr((word >> 16) & 0xFF))
        chars.append(chr((word >> 24) & 0xFF))
    return "".join(chars)


def xencode(text: str, key: str) -> str:
    """深澜私有 XTEA 变体（对应门户 JS xEncode）。"""
    if not text:
        return ""
    v = _to_words(text, True)
    k = _to_words(key, False)
    if len(k) < 4:
        k = k + [0] * (4 - len(k))

    n = len(v) - 1
    z = v[n]
    y = v[0]
    c = 0x9E3779B9
    q = 6 + 52 // (n + 1)
    d = 0
    p = 0

    while q > 0:
        q -= 1
        d = (d + c) & 0xFFFFFFFF
        e = (d >> 2) & 3
        for p in range(n):
            y = v[p + 1]
            m = ((z >> 5) ^ (y << 2)) & 0xFFFFFFFF
            m = (m + (((y >> 3) ^ (z << 4)) ^ (d ^ y))) & 0xFFFFFFFF
            m = (m + (k[(p & 3) ^ e] ^ z)) & 0xFFFFFFFF
            v[p] = (v[p] + m) & 0xFFFFFFFF
            z = v[p]
        # 注意：JS 的 for 循环结束后 p === n（而非 n-1），最后一块要用 p=n 取 k。
        p = n
        y = v[0]
        m = ((z >> 5) ^ (y << 2)) & 0xFFFFFFFF
        m = (m + (((y >> 3) ^ (z << 4)) ^ (d ^ y))) & 0xFFFFFFFF
        m = (m + (k[(p & 3) ^ e] ^ z)) & 0xFFFFFFFF
        v[n] = (v[n] + m) & 0xFFFFFFFF
        z = v[n]

    return _from_words(v)


def srun_base64(text: str) -> str:
    """门户 $.base64.encode 的等价实现（自定义字母表）。

    xEncode 返回的字符串每个字符都是 0-255 的字节，按 latin-1 取出后
    做标准 Base64，再按自定义字母表替换字符。
    """
    raw = text.encode("latin-1")
    std = _b64.b64encode(raw).decode("ascii")
    return std.translate(_ALPHA_MAP)


def hmac_md5_hex(password: str, token: str) -> str:
    """门户 js-md5 的 md5(password, token) —— 实为 HMAC-MD5(key=token)。"""
    return hmac.new(
        token.encode("utf-8"), password.encode("utf-8"), hashlib.md5
    ).hexdigest()


def make_info(username: str, password: str, ip: str, ac_id: str, token: str) -> str:
    """构造登录请求里的 info 字段（含密码密文）。"""
    payload = {
        "username": username,
        "password": password,
        "ip": ip,
        "acid": str(ac_id),
        "enc_ver": ENC_VER,
    }
    plain = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
    return "{SRBX1}" + srun_base64(xencode(plain, token))


def build_login_params(
    token: str,
    username: str,
    password: str,
    ac_id: str,
    ip: str,
    n: int = N,
    msg_type: int = _MSG_TYPE,
    os_name: str = "Windows 10",
    platform: str = "Windows",
    ignore: int = 2,
    double_stack: int = 0,
) -> dict:
    """按门户 $.Login 的顺序构造 srun_portal 请求参数（不含 callback）。"""
    hmd5 = hmac_md5_hex(password, token)
    info = make_info(username, password, ip, ac_id, token)
    chkstr = (
        token + username
        + token + hmd5
        + token + str(ac_id)
        + token + ip
        + token + str(n)
        + token + str(msg_type)
        + token + info
    )
    chksum = hashlib.sha1(chkstr.encode("utf-8")).hexdigest()
    return {
        "action": "login",
        "username": username,
        "password": "{MD5}" + hmd5,
        "ac_id": str(ac_id),
        "ip": ip,
        "chksum": chksum,
        "info": info,
        "n": str(n),
        "type": str(msg_type),
        "os": os_name,
        "name": platform,
        "double_stack": str(double_stack),
        "ignore": str(ignore),
    }


# ---------------------------------------------------------------------------
# HTTP / 门户交互
# ---------------------------------------------------------------------------

def parse_jsonp(text: str) -> dict:
    """解析 JSONP（或裸 JSON）返回。"""
    body = (text or "").strip().lstrip("\ufeff")
    if not body:
        raise SrunError("门户返回了空响应")
    if body[0] in "{[":
        return json.loads(body)
    match = _JSONP_RE.match(body)
    if not match:
        raise SrunError("无法解析门户响应: %r" % body[:200])
    return json.loads(match.group(1))


def _open(url: str, timeout: float):
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": _UA,
            "Accept": "*/*",
            "Accept-Encoding": "identity",
        },
    )
    # 门户在内网，强制绕过系统代理，避免被代理劫持。
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    return opener.open(req, timeout=timeout)


def _http_get(url: str, timeout: float = 6.0) -> tuple[str, str]:
    try:
        with _open(url, timeout) as resp:
            data = resp.read()
            charset = resp.headers.get_content_charset() or "utf-8"
            return resp.geturl(), data.decode(charset, errors="replace")
    except urllib.error.HTTPError as exc:
        # 例如返回 302/404 时也让调用方读取正文/位置。
        data = exc.read()
        charset = exc.headers.get_content_charset() or "utf-8"
        raise SrunError(
            "HTTP %s: %s" % (exc.code, data.decode(charset, errors="replace")[:200])
        ) from exc
    except (urllib.error.URLError, OSError, ValueError) as exc:
        raise SrunError("访问门户失败: %s" % exc) from exc


def _base_url(host: str, scheme: str = "http") -> str:
    host = host.strip().rstrip("/")
    if "://" in host:
        return host
    return "%s://%s" % (scheme, host)


def _jsonp(url: str, params: dict, timeout: float = 6.0) -> dict:
    params = dict(params)
    params.setdefault("callback", "cb")
    params.setdefault("_", str(int(time.time() * 1000)))
    query = urllib.parse.urlencode(params, quote_via=urllib.parse.quote_plus)
    full = url + ("&" if "?" in url else "?") + query
    _, text = _http_get(full, timeout)
    payload = parse_jsonp(text)
    if not isinstance(payload, dict):
        raise SrunError("门户返回格式异常: %r" % (payload,))
    return payload


def discover_portal(
    host: str, scheme: str = "http", timeout: float = 6.0
) -> dict:
    """访问门户首页，取 ac_id / 本机 IP（门户识别到的那一个）。

    返回 {"host":..., "url":..., "ac_id":..., "ip":...}；字段可能为空串。
    """
    base = _base_url(host, scheme)
    final_url, html = _http_get(base + "/", timeout)
    ac_id = ""
    ip = ""

    query = urllib.parse.urlsplit(final_url).query
    if query:
        qs = urllib.parse.parse_qs(query)
        ac_id = (qs.get("ac_id") or [""])[0]

    for pattern, name in (
        (r'id=["\']ac_id["\'][^>]*value=["\']([^"\']*)["\']', "ac_id"),
        (r'name=["\']ac_id["\'][^>]*value=["\']([^"\']*)["\']', "ac_id"),
        (r'id=["\']user_ip["\'][^>]*value=["\']([^"\']*)["\']', "ip"),
        (r'name=["\']user_ip["\'][^>]*value=["\']([^"\']*)["\']', "ip"),
    ):
        match = re.search(pattern, html, re.I)
        if match:
            if name == "ac_id" and not ac_id:
                ac_id = match.group(1).strip()
            elif name == "ip" and not ip:
                ip = match.group(1).strip()

    if not ip or ip in ("0.0.0.0", "127.0.0.1"):
        try:
            ip = local_ip_towards(host)
        except OSError:
            pass

    return {"host": host, "url": final_url, "ac_id": ac_id, "ip": ip}


def local_ip_towards(host: str) -> str:
    """取本机访问门户时使用的源 IP（不真正发包）。"""
    target = urllib.parse.urlsplit(_base_url(host)).hostname or host
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((target, 80))
        return sock.getsockname()[0]
    finally:
        sock.close()


def get_challenge(
    host: str, username: str, ip: str = "", scheme: str = "http",
    timeout: float = 6.0
) -> dict:
    url = _base_url(host, scheme) + "/cgi-bin/get_challenge"
    payload = _jsonp(url, {"username": username, "ip": ip}, timeout)
    if payload.get("error") != "ok" or not payload.get("challenge"):
        raise PortalError(
            "获取 challenge 失败: %s" % (payload.get("error_msg") or payload.get("error")),
            payload,
        )
    return payload


def _require_ascii(text: str, field: str) -> None:
    """凭据必须是 ASCII：门户 JS 的 xEncode 按 UTF-16 code unit 打包，
    与 Python 侧按 UTF-8 字节打包在非 ASCII 时不一致。学号/密码出现非 ASCII
    时直接报错，避免"看起来登录成功但实际失败"。"""
    try:
        text.encode("ascii")
    except UnicodeEncodeError as exc:
        raise SrunError(
            "%s 含非 ASCII 字符，当前协议实现不支持（请改成 ASCII 字符）" % field
        ) from exc


def portal_login(
    host: str,
    username: str,
    password: str,
    ac_id: str,
    ip: str = "",
    scheme: str = "http",
    timeout: float = 6.0,
    ignore: int = 2,
) -> dict:
    """执行一次完整登录。成功返回包含 error=ok 的字典；失败抛 PortalError。

    注意：门户在"该 IP 已经在线"时也会返回 error=ok，同时带
    suc_msg=ip_already_online_error；调用方应用 is_already_online() 区分。
    """
    if not username or not password:
        raise SrunError("用户名或密码为空")
    _require_ascii(username, "用户名")
    _require_ascii(password, "密码")

    challenge = get_challenge(host, username, ip, scheme, timeout)
    token = challenge["challenge"]
    client_ip = ip or challenge.get("client_ip") or ""
    if not client_ip:
        client_ip = local_ip_towards(host)

    params = build_login_params(
        token=token,
        username=username,
        password=password,
        ac_id=ac_id,
        ip=client_ip,
        ignore=ignore,
    )
    url = _base_url(host, scheme) + "/cgi-bin/srun_portal"
    payload = _jsonp(url, params, timeout)
    payload.setdefault("client_ip", client_ip)
    if payload.get("error") == "ok":
        return payload

    message = (
        payload.get("ploy_msg")
        or payload.get("error_msg")
        or payload.get("suc_msg")
        or payload.get("error")
        or "未知错误"
    )
    raise PortalError(str(message), payload)


def portal_logout(
    host: str, ac_id: str, ip: str = "", scheme: str = "http", timeout: float = 6.0
) -> dict:
    """注销当前 IP 的在线会话（用于清理"AC 仍认为在线"的残留会话）。"""
    url = _base_url(host, scheme) + "/cgi-bin/srun_portal"
    payload = _jsonp(
        url,
        {"action": "logout", "ac_id": str(ac_id), "ip": ip},
        timeout,
    )
    if payload.get("error") != "ok":
        raise PortalError(
            "注销会话失败: %s"
            % (payload.get("error_msg") or payload.get("error") or "未知错误"),
            payload,
        )
    return payload


def is_already_online(payload: dict) -> bool:
    """判断登录返回是否只是"该 IP 已在线"（并非真正重新认证成功）。"""
    if not isinstance(payload, dict):
        return False
    text = " ".join(
        str(payload.get(key) or "")
        for key in ("suc_msg", "error_msg", "ploy_msg", "error", "res")
    ).lower()
    return (
        "ip_already_online" in text
        or "already online" in text
        or "已在线" in text
    )


def get_online_info(host: str, scheme: str = "http", timeout: float = 5.0) -> dict:
    """查询本机在线状态（/cgi-bin/rad_user_info）。

    在线：{"error": "ok", "online": True, "user_name": ..., "online_ip": ...}
    离线：{"error": "not_online_error", "online": False, ...}
    """
    url = _base_url(host, scheme) + "/cgi-bin/rad_user_info"
    payload = _jsonp(url, {}, timeout)
    payload["online"] = bool(
        payload.get("error") == "ok"
        and payload.get("online_ip")
        and payload.get("online_ip") != "0.0.0.0"
    )
    return payload
