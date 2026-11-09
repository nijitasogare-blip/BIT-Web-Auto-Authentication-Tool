#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Windows DPAPI 加解密（当前用户作用域）。

密码用 CryptProtectData 加密后再写入 config.json，只有同一 Windows
用户在本机才能解密；即使配置文件被拷走也无法还原明文。
"""

from __future__ import annotations

import base64
import ctypes
import sys
from ctypes import wintypes

_ENTROPY = b"BITWebAutoLogin/v1"


class _DataBlob(ctypes.Structure):
    _fields_ = [
        ("cbData", wintypes.DWORD),
        ("pbData", ctypes.POINTER(ctypes.c_byte)),
    ]


if sys.platform == "win32":
    _crypt32 = ctypes.windll.crypt32
    _kernel32 = ctypes.windll.kernel32

    _crypt32.CryptProtectData.argtypes = [
        ctypes.POINTER(_DataBlob),
        wintypes.LPCWSTR,
        ctypes.POINTER(_DataBlob),
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(_DataBlob),
    ]
    _crypt32.CryptProtectData.restype = wintypes.BOOL
    _crypt32.CryptUnprotectData.argtypes = [
        ctypes.POINTER(_DataBlob),
        ctypes.POINTER(wintypes.LPWSTR),
        ctypes.POINTER(_DataBlob),
        ctypes.c_void_p,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.POINTER(_DataBlob),
    ]
    _crypt32.CryptUnprotectData.restype = wintypes.BOOL
    _kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    _kernel32.LocalFree.restype = ctypes.c_void_p


def _blob(data: bytes) -> _DataBlob:
    buf = ctypes.create_string_buffer(data)
    return _DataBlob(
        len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_byte))
    )


def _entropy_blob() -> _DataBlob:
    return _blob(_ENTROPY)


def protect(data: bytes) -> str:
    """返回 DPAPI 加密后的 Base64 字符串。"""
    if sys.platform != "win32":
        raise OSError("DPAPI 仅支持 Windows")
    blob_in = _blob(data)
    blob_entropy = _entropy_blob()
    blob_out = _DataBlob()
    ok = _crypt32.CryptProtectData(
        ctypes.byref(blob_in),
        "BITWebAutoLogin",
        ctypes.byref(blob_entropy),
        None,
        None,
        0,
        ctypes.byref(blob_out),
    )
    if not ok:
        raise ctypes.WinError()
    try:
        raw = ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        _kernel32.LocalFree(blob_out.pbData)
    return base64.b64encode(raw).decode("ascii")


def unprotect(token_b64: str) -> bytes:
    """解密 protect() 产生的 Base64 字符串。"""
    if sys.platform != "win32":
        raise OSError("DPAPI 仅支持 Windows")
    raw = base64.b64decode(token_b64)
    blob_in = _blob(raw)
    blob_entropy = _entropy_blob()
    blob_out = _DataBlob()
    ok = _crypt32.CryptUnprotectData(
        ctypes.byref(blob_in),
        None,
        ctypes.byref(blob_entropy),
        None,
        None,
        0,
        ctypes.byref(blob_out),
    )
    if not ok:
        raise ctypes.WinError()
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        _kernel32.LocalFree(blob_out.pbData)
