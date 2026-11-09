#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""srun_client 的离线单元测试。

测试向量由 tests/js_reference.js 独立生成（直接照抄门户下发的 JS 函数并
用 Node 的 crypto 计算 HMAC/SHA1），确保 Python 实现与门户前端逐位一致。
运行：
    python -m unittest discover -s tests -v     (项目根目录下)
"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import srun_client as srun  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "js_reference_expected.json")


class ProtocolVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(FIXTURE, "r", encoding="utf-8") as fh:
            cls.vectors = json.load(fh)

    def test_xencode(self):
        for case in self.vectors["xencode"]:
            with self.subTest(text=case["text"][:40], key=case["key"][:12]):
                self.assertEqual(srun.xencode(case["text"], case["key"]), case["expected"])

    def test_custom_base64(self):
        for case in self.vectors["base64"]:
            with self.subTest(bytes=case["bytes"][:24]):
                text = bytes.fromhex(case["bytes"]).decode("latin-1")
                self.assertEqual(srun.srun_base64(text), case["expected"])

    def test_hmac_md5(self):
        for case in self.vectors["hmac"]:
            with self.subTest(password=case["password"][:12]):
                self.assertEqual(
                    srun.hmac_md5_hex(case["password"], case["token"]),
                    case["expected"],
                )

    def test_build_login_params(self):
        for case in self.vectors["login"]:
            with self.subTest(user=case["username"], token=case["token"][:8]):
                params = srun.build_login_params(
                    token=case["token"],
                    username=case["username"],
                    password=case["password"],
                    ac_id=case["ac_id"],
                    ip=case["ip"],
                )
                self.assertEqual(params, case["params"])

    def test_make_info_prefix(self):
        info = srun.make_info("u", "p", "10.0.0.2", "43", "tok")
        self.assertTrue(info.startswith("{SRBX1}"))
        self.assertNotIn("password", info)  # 密码在信息字段中已被加密

    def test_already_online_detection(self):
        self.assertTrue(
            srun.is_already_online(
                {"error": "ok", "suc_msg": "ip_already_online_error"}
            )
        )
        self.assertTrue(
            srun.is_already_online({"error": "error_msg", "error_msg": "IP 已在线"})
        )
        self.assertFalse(srun.is_already_online({"error": "ok"}))
        self.assertFalse(srun.is_already_online({}))

    def test_non_ascii_credentials_rejected(self):
        with self.assertRaises(srun.SrunError):
            srun.portal_login(
                "127.0.0.1:1", "学号", "password", "43", timeout=0.2
            )


class JsonpParsing(unittest.TestCase):
    def test_jsonp(self):
        self.assertEqual(
            srun.parse_jsonp('dr1003({"error":"ok","challenge":"abc"});'),
            {"error": "ok", "challenge": "abc"},
        )

    def test_jsonp_with_dots(self):
        self.assertEqual(
            srun.parse_jsonp('jQuery123_456({"error": "ok"})'),
            {"error": "ok"},
        )

    def test_plain_json(self):
        self.assertEqual(srun.parse_jsonp('{"error":"ok"}'), {"error": "ok"})

    def test_empty(self):
        with self.assertRaises(srun.SrunError):
            srun.parse_jsonp("   ")
        with self.assertRaises(srun.SrunError):
            srun.parse_jsonp("not a jsonp response")


if __name__ == "__main__":
    unittest.main(verbosity=2)
