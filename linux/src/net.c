/* linux/src/net.c - 外网连通性检查：HTTP 优先 + ping 命令兜底（只读） */
#define _GNU_SOURCE
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

static int text_contains_portal(const char *text, const wchar_t *portal_host) {
    if (!text) return 0;
    if (strstr(text, "srun_portal")) return 1;
    if (strstr(text, "eportal")) return 1;
    if (strstr(text, "网络准入认证")) return 1;
    if (strstr(text, "深澜")) return 1;
    if (portal_host && portal_host[0]) {
        char host_utf8[256];
        bw_wide_to_utf8(portal_host, host_utf8, sizeof(host_utf8));
        if (host_utf8[0] && strstr(text, host_utf8)) return 1;
    }
    return 0;
}

static int wtext_contains_portal(const wchar_t *text, const wchar_t *portal_host) {
    char utf8[8192];
    bw_wide_to_utf8(text ? text : L"", utf8, sizeof(utf8));
    return text_contains_portal(utf8, portal_host);
}

static int http_probe_online(const BWConfig *cfg, int timeout_ms,
                             wchar_t *detail, int cap) {
    BWHttp resp;
    if (!bw_http_get(cfg->http_probe_url, timeout_ms, 0, &resp)) {
        _snwprintf(detail, cap - 1, L"HTTP 探测失败（无法建立连接）");
        detail[cap - 1] = 0;
        return 0;
    }
    int online = 0;
    if (resp.status == 200 && resp.body_len > 0) {
        if (text_contains_portal(resp.body, cfg->portal_host)) {
            _snwprintf(detail, cap - 1, L"HTTP 200 但返回的是认证门户页面");
        } else {
            online = 1;
            _snwprintf(detail, cap - 1, L"HTTP 200");
        }
    } else if (resp.status >= 300 && resp.status < 400) {
        if (wtext_contains_portal(resp.location, cfg->portal_host) ||
            text_contains_portal(resp.body, cfg->portal_host)) {
            _snwprintf(detail, cap - 1, L"被重定向到认证门户（HTTP %d）", resp.status);
        } else {
            online = 1;
            _snwprintf(detail, cap - 1, L"HTTP %d 跳转到外网", resp.status);
        }
    } else {
        _snwprintf(detail, cap - 1, L"HTTP %d", resp.status);
    }
    detail[cap - 1] = 0;
    bw_http_free(&resp);
    return online;
}

static int ping_host(const wchar_t *host, int timeout_sec) {
    char host_utf8[256];
    bw_wide_to_utf8(host, host_utf8, sizeof(host_utf8));
    if (!host_utf8[0]) return 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "/bin/ping -c 1 -W %d %s 2>/dev/null", timeout_sec, host_utf8);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char out[4096];
    int n = 0, c;
    while ((c = fgetc(fp)) != EOF && n < (int)sizeof(out) - 1) out[n++] = (char)c;
    out[n] = 0;
    int rc = pclose(fp);
    if (rc != 0) return 0;
    return strstr(out, "1 received") != NULL || strstr(out, "1 packets received") != NULL ||
           strstr(out, "ttl=") != NULL || strstr(out, "TTL=") != NULL;
}

static int connectivity_once(const BWConfig *cfg, wchar_t *detail, int cap, int quick) {
    detail[0] = 0;
    if (cfg->http_probe) {
        int timeout_ms = (cfg->http_probe_timeout_sec > 0 ? cfg->http_probe_timeout_sec : 5) * 1000;
        if (quick && timeout_ms > 3000) timeout_ms = 3000;
        if (http_probe_online(cfg, timeout_ms, detail, cap)) return 1;
    }
    int timeout_sec = cfg->ping_timeout_ms > 0 ? cfg->ping_timeout_ms / 1000 : 2;
    if (timeout_sec < 1) timeout_sec = 1;
    if (quick && timeout_sec > 1) timeout_sec = 1;
    for (int i = 0; i < cfg->ping_target_count; i++) {
        if (ping_host(cfg->ping_targets[i], timeout_sec)) {
            _snwprintf(detail, cap - 1, L"ping %ls 正常", cfg->ping_targets[i]);
            detail[cap - 1] = 0;
            return 1;
        }
    }
    if (!detail[0]) {
        if (cfg->ping_target_count > 0) {
            _snwprintf(detail, cap - 1, L"ping %ls 无响应", cfg->ping_targets[0]);
        } else {
            _snwprintf(detail, cap - 1, L"连通性检测失败");
        }
        detail[cap - 1] = 0;
    }
    return 0;
}

int bw_connectivity_check(const BWConfig *cfg, wchar_t *detail, int cap) {
    if (connectivity_once(cfg, detail, cap, 0)) return 1;
    bw_sleep_ms(600);
    return connectivity_once(cfg, detail, cap, 1);
}
