#!/usr/bin/env python3
"""本地假深澜门户，用于 Linux C 客户端登录链路测试。

校验方式与真实服务端一致：先用提交字段重算 chksum，再校验密码摘要。
"""
import hashlib
import hmac
import json
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = "0123456789abcdef" * 2
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
USER = sys.argv[2] if len(sys.argv) > 2 else "2024000001"
PASSWORD = sys.argv[3] if len(sys.argv) > 3 else "TestP@ss!234"
LOG = sys.argv[4] if len(sys.argv) > 4 else "/tmp/bwtest/portal_requests.log"


def log_request(path):
    with open(LOG, "a", encoding="utf-8") as fh:
        fh.write(path + "\n")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_text(self, text, status=200):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        log_request(parsed.path)
        if parsed.path == "/":
            self.send_text('<html><input type="hidden" id="ac_id" value="43">'
                           '<input type="hidden" id="user_ip" value="127.0.0.1">'
                           '<title>网络准入认证 深澜</title></html>')
            return
        if parsed.path == "/probe":
            # 故意返回认证门户页面，让客户端判断为“离线”，用于测试自动登录
            self.send_text("<html>srun_portal 网络准入认证 深澜</html>")
            return
        if parsed.path == "/cgi-bin/get_challenge":
            payload = {"challenge": TOKEN, "client_ip": "127.0.0.1",
                       "ecode": 0, "error": "ok", "res": "ok"}
            cb = query.get("callback", ["cb"])[0]
            self.send_text("%s(%s)" % (cb, json.dumps(payload)))
            return
        if parsed.path == "/cgi-bin/srun_portal":
            params = {k: v[0] for k, v in query.items()}
            cb = params.get("callback", "cb")
            action = params.get("action", "login")
            if action == "logout":
                self.send_text('%s({"error":"ok","res":"ok"})' % cb)
                return
            hmd5 = params.get("password", "").replace("{MD5}", "")
            chk = (
                TOKEN + params.get("username", "") + TOKEN + hmd5 + TOKEN +
                params.get("ac_id", "") + TOKEN + params.get("ip", "") + TOKEN +
                params.get("n", "") + TOKEN + params.get("type", "") + TOKEN +
                params.get("info", "")
            )
            if params.get("chksum") != hashlib.sha1(chk.encode()).hexdigest():
                self.send_text('%s({"error":"sign_error","res":"sign_error"})' % cb)
                return
            expect = hmac.new(TOKEN.encode(), PASSWORD.encode(), hashlib.md5).hexdigest()
            if hmd5 != expect:
                self.send_text('%s({"error":"login_error","error_msg":"密码错误","ecode":0})' % cb)
                return
            self.send_text('%s({"error":"ok","res":"ok","online_ip":"127.0.0.1"})' % cb)
            return
        if parsed.path == "/cgi-bin/rad_user_info":
            self.send_text('%s({"error":"not_online_error","res":"not_online_error"})'
                           % query.get("callback", ["cb"])[0])
            return
        self.send_text("not found", 404)


if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("PORT", PORT, flush=True)
    server.serve_forever()