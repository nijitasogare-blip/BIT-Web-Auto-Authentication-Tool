#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bitweb_monitor / dpapi / appconfig 的单元测试（不访问网络）。"""

import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import appconfig  # noqa: E402
import bitweb_monitor as monitor  # noqa: E402
import dpapi  # noqa: E402

ENGLISH_OUTPUT = """
There is 1 interface on the system:

    Name                   : WLAN
    Description            : Realtek 8922AE WiFi 7 PCI-E NIC
    GUID                   : 80fedd4b-2f24-4cc2-bcbd-c633373860e1
    Physical address       : aa:bb:cc:dd:ee:ff
    Interface type         : Primary
    State                  : connected
    SSID                   : BIT-Web
    AP BSSID               : 11:22:33:44:55:66
    Band                   : 5 GHz
    Channel                : 52
    Network type           : Infrastructure
    Radio type             : 802.11ax
    Authentication         : Open
    Cipher                 : None
    Connection mode        : Auto Connect
    Receive rate (Mbps)    : 573.5
    Transmit rate (Mbps)   : 573.5
    Signal                 : 85%
    Profile                : BIT-Web
"""

CHINESE_OUTPUT = """
系统上有 1 个接口:

    名称                   : WLAN
    描述                   : Realtek 8922AE WiFi 7 PCI-E NIC
    GUID                   : 80fedd4b-2f24-4cc2-bcbd-c633373860e1
    物理地址               : aa:bb:cc:dd:ee:ff
    接口类型               : 主要
    状态                   : 已连接
    SSID                   : BIT-Web
    AP BSSID               : 11:22:33:44:55:66
    频带                   : 5 GHz
    通道                   : 52
    信号                   : 85%
"""

MULTI_DISCONNECTED = """
There are 2 interfaces on the system:

    Name                   : WLAN
    Description            : Wi-Fi
    State                  : disconnected
    Radio status           : Hardware On
    Software off           : 0

    Name                   : WLAN 2
    Description            : Wi-Fi 2
    State                  : connected
    SSID                   : BIT-Mobile
    AP BSSID               : aa:bb:cc:dd:ee:ff
    Signal                 : 60%
"""


class WlanParsing(unittest.TestCase):
    def test_english(self):
        items = monitor.parse_wlan_interfaces(ENGLISH_OUTPUT)
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["name"], "WLAN")
        self.assertEqual(items[0]["ssid"], "BIT-Web")
        self.assertTrue(items[0]["connected"])

    def test_chinese(self):
        items = monitor.parse_wlan_interfaces(CHINESE_OUTPUT)
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["name"], "WLAN")
        self.assertEqual(items[0]["ssid"], "BIT-Web")
        self.assertTrue(items[0]["connected"])

    def test_disconnected_and_other_ssid(self):
        items = monitor.parse_wlan_interfaces(MULTI_DISCONNECTED)
        self.assertEqual(len(items), 2)
        self.assertFalse(items[0]["connected"])
        self.assertEqual(items[0]["ssid"], "")
        self.assertTrue(items[1]["connected"])
        self.assertEqual(items[1]["ssid"], "BIT-Mobile")

    def test_no_wlan(self):
        self.assertEqual(monitor.parse_wlan_interfaces(""), [])
        self.assertEqual(monitor.parse_wlan_interfaces("WLAN service is not running"), [])

    def test_bssid_is_not_ssid(self):
        output = """
    Name                   : WLAN
    State                  : disconnected
    AP BSSID               : 11:22:33:44:55:66
"""
        items = monitor.parse_wlan_interfaces(output)
        self.assertEqual(items[0]["ssid"], "")
        self.assertFalse(items[0]["connected"])


class SsidMatching(unittest.TestCase):
    def test_exact_and_case_sensitive(self):
        allowed = ["BIT-Web"]
        self.assertTrue(monitor.ssid_allowed("BIT-Web", allowed))
        # 大小写/多余字符都不算，避免连到仿冒 SSID 时泄露账号
        self.assertFalse(monitor.ssid_allowed("bit-web", allowed))
        self.assertFalse(monitor.ssid_allowed("BIT-Web-5G", allowed))
        self.assertFalse(monitor.ssid_allowed("Neighbor-WiFi-5G", allowed))
        self.assertFalse(monitor.ssid_allowed("", allowed))


class AuthErrorClassification(unittest.TestCase):
    def test_credential_errors(self):
        for message in ("密码错误", "用户名或密码不正确", "Password error",
                        "账号已被冻结", "用户不存在"):
            self.assertTrue(monitor._is_auth_error(message), message)

    def test_transient_errors(self):
        for message in ("网络异常", "AC 未响应", "challenge 失败"):
            self.assertFalse(monitor._is_auth_error(message), message)


class ConfigHandling(unittest.TestCase):
    def test_default_merge_and_save(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config.json"
            self.assertTrue(appconfig.ensure_config(path))
            config = appconfig.load_config(path)
            self.assertEqual(config["network"]["allowed_ssids"], ["BIT-Web"])
            config["username"] = "test-user"
            config["network"]["ssid_poll_sec"] = 5
            appconfig.save_config(path, config)
            reloaded = appconfig.load_config(path)
            self.assertEqual(reloaded["username"], "test-user")
            self.assertEqual(reloaded["network"]["ssid_poll_sec"], 5)
            # 缺失的字段应被默认值补齐
            self.assertEqual(reloaded["connectivity"]["check_interval_sec"], 300)

    def test_relative_log_path(self):
        base = Path("C:/app/config.json")
        self.assertEqual(
            appconfig.resolve_relative(base, "logs/x.log"),
            Path("C:/app/logs/x.log"),
        )


@unittest.skipUnless(sys.platform == "win32", "DPAPI 仅 Windows 可用")
class DpapiRoundtrip(unittest.TestCase):
    def test_roundtrip(self):
        secret = "p@ssw0rd-测试-123"
        token = dpapi.protect(secret.encode("utf-8"))
        self.assertNotIn(secret, token)
        self.assertEqual(dpapi.unprotect(token).decode("utf-8"), secret)

    def test_roundtrip_binary(self):
        token = dpapi.protect(b"secret")
        raw = dpapi.unprotect(token)
        self.assertEqual(raw, b"secret")


class MonitorStateMachine(unittest.TestCase):
    """状态机测试：确保"非白名单 SSID 时绝不触发任何门户/登录请求"。"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.config_path = Path(self.tmp.name) / "config.json"
        appconfig.save_config(self.config_path, appconfig.DEFAULT_CONFIG)
        self.config = appconfig.load_config(self.config_path)
        self.logger = __import__("logging").getLogger("bitweb-test")
        self.logger.addHandler(__import__("logging").NullHandler())

    def _monitor(self, **kwargs):
        return monitor.Monitor(
            config=self.config,
            config_path=self.config_path,
            logger=self.logger,
            **kwargs,
        )

    def test_other_ssid_never_touches_network(self):
        m = self._monitor(simulate_ssid="Neighbor-WiFi-5G")
        with mock.patch.object(monitor, "check_connectivity") as checker, \
                mock.patch.object(monitor, "attempt_login") as login:
            for _ in range(5):
                m.tick()
        checker.assert_not_called()
        login.assert_not_called()
        self.assertEqual(m.mode, "sleep")

    def test_bssid_only_no_connectivity(self):
        # 没有无线网卡 / 未连接：同样不应触发网络请求
        m = self._monitor()
        with mock.patch.object(monitor, "get_wlan_interfaces", return_value=([], "")), \
                mock.patch.object(monitor, "check_connectivity") as checker:
            m.tick()
        checker.assert_not_called()
        self.assertEqual(m.mode, "sleep")

    def test_wake_on_bitweb_and_check(self):
        m = self._monitor(simulate_ssid="BIT-Web")
        with mock.patch.object(monitor, "check_connectivity", return_value=(True, "ok")) as checker:
            m.tick()
        checker.assert_called_once()
        self.assertEqual(m.mode, "active")

    def test_sleep_after_confirm_polls(self):
        m = self._monitor(simulate_ssid="BIT-Web")
        with mock.patch.object(monitor, "check_connectivity", return_value=(True, "ok")):
            m.tick()
        self.assertEqual(m.mode, "active")
        m.simulate_ssid = "OtherWiFi"
        with mock.patch.object(monitor, "check_connectivity", return_value=(True, "ok")):
            m.tick()
            self.assertEqual(m.mode, "active")   # 第一次丢失只进入 pending
            m.tick()
            self.assertEqual(m.mode, "sleep")    # 第二次确认后才休眠

    def test_offline_login_then_auth_error_sets_cooldown(self):
        m = self._monitor(simulate_ssid="BIT-Web", simulate_offline=True)
        with mock.patch.object(monitor, "attempt_login", return_value="auth_error") as login:
            m.tick()
        self.assertGreater(m.auth_fail_until, time.monotonic())
        login.assert_called_once()  # 认证类错误不在本次流程内重试

    def test_config_change_resets_cooldown(self):
        m = self._monitor(simulate_ssid="OtherWiFi")
        m.auth_fail_until = time.monotonic() + 1800
        time.sleep(0.01)
        config = appconfig.load_config(self.config_path)
        config["username"] = "updated-user"
        appconfig.save_config(self.config_path, config)
        m.tick()
        self.assertEqual(m.auth_fail_until, 0.0)
        self.assertEqual(m.config["username"], "updated-user")

    def test_unrelated_config_change_keeps_cooldown(self):
        m = self._monitor(simulate_ssid="OtherWiFi")
        deadline = time.monotonic() + 1800
        m.auth_fail_until = deadline
        time.sleep(0.01)
        config = appconfig.load_config(self.config_path)
        config["logging"]["level"] = "DEBUG"
        appconfig.save_config(self.config_path, config)
        m.tick()
        self.assertEqual(m.auth_fail_until, deadline)

    def test_dry_run_works_without_password(self):
        config = appconfig.load_config(self.config_path)
        config["username"] = "2024000001"
        with mock.patch.object(
            monitor.srun_client,
            "discover_portal",
            return_value={"ac_id": "43", "ip": "10.0.0.2", "url": "http://x/"},
        ):
            result = monitor.attempt_login(config, self.logger, dry_run=True)
        self.assertEqual(result, "dry_run")

    def test_no_password_sets_config_error_cooldown(self):
        m = self._monitor(simulate_ssid="BIT-Web", simulate_offline=True)
        with mock.patch.object(monitor, "attempt_login", return_value="no_password"):
            m.tick()
        self.assertGreater(m.config_error_until, time.monotonic())


class ConnectivityHelpers(unittest.TestCase):
    def test_portal_text_detection(self):
        self.assertTrue(monitor._is_portal_text("http://10.0.0.55/srun_portal_pc", "10.0.0.55"))
        self.assertTrue(monitor._is_portal_text("请进行网络准入认证", "10.0.0.55"))
        self.assertFalse(monitor._is_portal_text("https://www.baidu.com/", "10.0.0.55"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
