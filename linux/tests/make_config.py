#!/usr/bin/env python3
"""生成 Linux 测试配置（仅本地测试用）。"""
import json
import sys

out = sys.argv[1]
portal = sys.argv[2] if len(sys.argv) > 2 else "10.0.0.55"
username = sys.argv[3] if len(sys.argv) > 3 else "2024000001"
password = sys.argv[4] if len(sys.argv) > 4 else "TestP@ss!234"
allowed = sys.argv[5] if len(sys.argv) > 5 else "BIT-Web"
probe_url = sys.argv[6] if len(sys.argv) > 6 else ("http://" + portal + "/probe")

host = portal
port = 80
if ":" in portal:
    host, p = portal.rsplit(":", 1)
    port = int(p)

cfg = {
    "username": username,
    "password": password,
    "portal": {"host": portal, "scheme": "http", "ac_id": "", "timeout_sec": 6},
    "network": {"allowed_ssids": [allowed], "ssid_poll_sec": 5, "sleep_confirm_polls": 2},
    "connectivity": {
        "check_interval_sec": 300,
        "ping_targets": ["invalid.invalid"],
        "ping_timeout_ms": 500,
        "http_probe": True,
        "http_probe_url": probe_url,
        "http_probe_timeout_sec": 2,
    },
    "login": {"retry_count": 1, "retry_delay_sec": 1, "auth_error_backoff_sec": 60, "ignore": 2},
    "logging": {"level": "DEBUG", "file": "/tmp/bwtest/log.txt", "max_bytes": 1048576, "backup_count": 2},
}
with open(out, "w", encoding="utf-8") as fh:
    json.dump(cfg, fh, ensure_ascii=False, indent=2)
print("written", out)