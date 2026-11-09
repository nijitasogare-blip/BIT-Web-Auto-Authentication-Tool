#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地集成测试：用假门户（仅 127.0.0.1）验证"断网 -> 自动登录 -> 验证"完整链路。

不访问真实校园网；密码用 DPAPI 加密后写入临时配置，与真实运行路径一致。
"""

import hashlib
import http.server
import json
import logging
import os
import socket
import sys
import tempfile
import threading
import unittest
import urllib.parse
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import appconfig  # noqa: E402
import bitweb_monitor as monitor  # noqa: E402
import dpapi  # noqa: E402
import srun_client  # noqa: E402

USERNAME = "2024000001"
PASSWORD = "local-test-password"


class FakePortal(http.server.BaseHTTPRequestHandler):
    requests = []
    challenge = "0123456789abcdef" * 4
    already_online_pending = False
    logged_out = False

    def log_message(self, *args):  # 静默
        pass

    def _send(self, text: str, status: int = 200):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        FakePortal.requests.append((parsed.path, query))

        if parsed.path == "/":
            self._send(
                '<html><input type="hidden" id="ac_id" value="43">'
                '<input type="hidden" id="user_ip" value="127.0.0.1"></html>'
            )
            return

        if parsed.path == "/cgi-bin/get_challenge":
            payload = {
                "challenge": FakePortal.challenge,
                "client_ip": "127.0.0.1",
                "ecode": 0,
                "error": "ok",
                "res": "ok",
            }
            callback = query.get("callback", ["cb"])[0]
            self._send("%s(%s)" % (callback, json.dumps(payload)))
            return

        if parsed.path == "/cgi-bin/srun_portal":
            params = {key: value[0] for key, value in query.items()}
            callback = params.get("callback", "cb")
            action = params.get("action", "login")
            if action == "logout":
                FakePortal.logged_out = True
                FakePortal.already_online_pending = False
                self._send('%s({"error":"ok","res":"ok"})' % callback)
                return
            # 与真实 Srun 服务端一样：先用"提交上来的字段"重算 chksum 验证签名，
            # 再单独校验密码摘要是否正确。
            submitted_hmd5 = params.get("password", "").replace("{MD5}", "")
            chkstr = (
                FakePortal.challenge + params.get("username", "")
                + FakePortal.challenge + submitted_hmd5
                + FakePortal.challenge + params.get("ac_id", "")
                + FakePortal.challenge + params.get("ip", "")
                + FakePortal.challenge + params.get("n", "")
                + FakePortal.challenge + params.get("type", "")
                + FakePortal.challenge + params.get("info", "")
            )
            if params.get("chksum") != hashlib.sha1(chkstr.encode("utf-8")).hexdigest():
                self._send('%s({"error":"sign_error","res":"sign_error"})' % callback)
                return
            expected_hmd5 = srun_client.hmac_md5_hex(PASSWORD, FakePortal.challenge)
            if submitted_hmd5 != expected_hmd5:
                self._send(
                    '%s({"error":"login_error","error_msg":"密码错误","ecode":0})'
                    % callback
                )
                return
            if FakePortal.already_online_pending:
                FakePortal.already_online_pending = False
                self._send(
                    '%s({"error":"ok","res":"ok","suc_msg":"ip_already_online_error",'
                    '"online_ip":"127.0.0.1"})' % callback
                )
                return
            self._send(
                '%s({"error":"ok","res":"ok","online_ip":"127.0.0.1"})' % callback
            )
            return

        self._send("not found", 404)


def _start_server():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakePortal)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


@unittest.skipUnless(sys.platform == "win32", "本地集成测试需要 Windows DPAPI")
class LocalLoginFlow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = _start_server()
        cls.port = cls.server.server_address[1]
        cls.host = "127.0.0.1:%d" % cls.port
        FakePortal.requests.clear()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        FakePortal.already_online_pending = False
        FakePortal.logged_out = False
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.config_path = Path(self.tmp.name) / "config.json"
        config = appconfig.load_config(self.config_path)  # 默认配置
        config["username"] = USERNAME
        config["portal"]["host"] = self.host
        config["portal"]["ac_id"] = ""
        config["login"]["retry_count"] = 0
        config["login"]["retry_delay_sec"] = 0
        config["logging"]["file"] = str(Path(self.tmp.name) / "test.log")
        self.config = config
        FakePortal.requests.clear()

    def _logger(self):
        logger = logging.getLogger("bitweb-integration-test")
        logger.handlers.clear()
        logger.addHandler(logging.NullHandler())
        logger.setLevel(logging.DEBUG)
        return logger

    def test_discover_portal_parses_ac_id_and_ip(self):
        discovered = srun_client.discover_portal(self.host, "http", 3)
        self.assertEqual(discovered["ac_id"], "43")
        self.assertEqual(discovered["ip"], "127.0.0.1")

    def test_attempt_login_success(self):
        self.config["password_dpapi"] = dpapi.protect(PASSWORD.encode())
        result = monitor.attempt_login(self.config, self._logger())
        self.assertEqual(result, "ok")
        login_calls = [q for p, q in FakePortal.requests if p == "/cgi-bin/srun_portal"]
        self.assertEqual(len(login_calls), 1)

    def test_attempt_login_wrong_password_is_auth_error(self):
        self.config["password_dpapi"] = dpapi.protect(b"wrong-password")
        result = monitor.attempt_login(self.config, self._logger())
        self.assertEqual(result, "auth_error")

    def test_attempt_login_without_password(self):
        result = monitor.attempt_login(self.config, self._logger())
        self.assertEqual(result, "no_password")

    def test_monitor_offline_then_login_recovers(self):
        self.config["password_dpapi"] = dpapi.protect(PASSWORD.encode())
        appconfig.save_config(self.config_path, self.config)
        cfg = appconfig.load_config(self.config_path)
        m = monitor.Monitor(
            config=cfg,
            config_path=self.config_path,
            logger=self._logger(),
            simulate_ssid="BIT-Web",
            simulate_offline=True,
        )
        online = {"value": False}

        def fake_check(_config):
            # 第一次（断网）False；登录后验证时 True
            if not online["value"]:
                online["value"] = True
                return False, "模拟断网"
            return True, "ok"

        with mock.patch.object(monitor, "check_connectivity", side_effect=fake_check), \
                mock.patch.object(monitor.time, "sleep", return_value=None):
            m.tick()

        self.assertEqual(m.mode, "active")
        login_calls = [q for p, q in FakePortal.requests if p == "/cgi-bin/srun_portal"]
        self.assertEqual(len(login_calls), 1)
        self.assertGreater(m.next_check, 0)

    def test_already_online_triggers_logout_and_relogin(self):
        self.config["password_dpapi"] = dpapi.protect(PASSWORD.encode())
        appconfig.save_config(self.config_path, self.config)
        cfg = appconfig.load_config(self.config_path)
        m = monitor.Monitor(
            config=cfg,
            config_path=self.config_path,
            logger=self._logger(),
            simulate_ssid="BIT-Web",
            simulate_offline=True,
        )
        FakePortal.already_online_pending = True

        with mock.patch.object(monitor, "check_connectivity", return_value=(True, "ok")), \
                mock.patch.object(monitor.time, "sleep", return_value=None):
            m.tick()

        self.assertEqual(m.mode, "active")
        actions = [q.get("action", ["login"])[0] for p, q in FakePortal.requests
                   if p == "/cgi-bin/srun_portal"]
        self.assertEqual(actions.count("logout"), 1)
        self.assertEqual(actions.count("login"), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
