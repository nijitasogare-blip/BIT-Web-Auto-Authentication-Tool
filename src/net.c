/* net.c - 外网连通性检查：HTTP(Socket) 优先 + ICMP ping 兜底
 *
 * 外网探测使用自带 Winsock HTTP 客户端（只发 IPv4、完全可控），
 * 避免 WinHTTP 在这类校园网络下偶发 connect 超时；门户内网请求仍用 WinHTTP。
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "bitweb.h"
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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
    char utf8[4096];
    bw_wide_to_utf8(text ? text : L"", utf8, sizeof(utf8));
    return text_contains_portal(utf8, portal_host);
}

static int parse_http_url(const wchar_t *url, char *host, int hostcap,
                          int *port, char *path, int pathcap) {
    char utf8[2048];
    bw_wide_to_utf8(url, utf8, sizeof(utf8));
    if (_strnicmp(utf8, "http://", 7) != 0) return 0;
    const char *p = utf8 + 7;
    const char *slash = strchr(p, '/');
    char authority[512];
    size_t alen = slash ? (size_t)(slash - p) : strlen(p);
    if (alen >= sizeof(authority)) return 0;
    memcpy(authority, p, alen);
    authority[alen] = 0;
    *port = 80;
    char *colon = strrchr(authority, ':');
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) *port = 80;
    }
    if (!authority[0]) return 0;
    strncpy(host, authority, hostcap - 1);
    host[hostcap - 1] = 0;
    const char *pathpart = slash ? slash : "/";
    strncpy(path, pathpart, pathcap - 1);
    path[pathcap - 1] = 0;
    return 1;
}

static int connect_timeout(SOCKET s, const struct sockaddr *addr, int addrlen, int ms) {
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
    int rc = connect(s, addr, addrlen);
    if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        rc = select(0, NULL, &wf, NULL, &tv);
        if (rc > 0) {
            int err = 0;
            int elen = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
            rc = (err == 0) ? 0 : -1;
        } else {
            rc = -1;
        }
    } else if (rc == 0) {
        rc = 0;
    } else {
        rc = -1;
    }
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    return rc;
}

/* 返回 1 表示成功拿到 HTTP 响应 */
static int simple_http_get(const wchar_t *url, int timeout_ms,
                           char *body, int bodycap, int *out_status,
                           wchar_t *location, int loccap) {
    *out_status = 0;
    if (location && loccap > 0) location[0] = 0;
    char host[256], path[1024];
    int port = 80;
    if (!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path))) return 0;

    WSADATA wsa;
    static int wsa_ready = 0;
    if (!wsa_ready) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        wsa_ready = 1;
    }
    ADDRINFOW hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    wchar_t whost[256];
    bw_utf8_to_wide(host, whost, 256);
    PADDRINFOW result = NULL;
    if (GetAddrInfoW(whost, NULL, &hints, &result) != 0 || !result) return 0;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { FreeAddrInfoW(result); return 0; }
    struct sockaddr_in addr;
    memcpy(&addr, result->ai_addr, sizeof(addr));
    addr.sin_port = htons((u_short)port);
    FreeAddrInfoW(result);
    if (connect_timeout(s, (struct sockaddr *)&addr, sizeof(addr), timeout_ms) != 0) {
        closesocket(s);
        return 0;
    }
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    char request[2048];
    _snprintf(request, sizeof(request),
              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: BITWebAutoLogin/%ls\r\n"
              "Accept: */*\r\nConnection: close\r\n\r\n",
              path, host, BITWEB_VERSION);
    request[sizeof(request) - 1] = 0;
    if (send(s, request, (int)strlen(request), 0) <= 0) {
        closesocket(s);
        return 0;
    }

    int total = 0, truncated = 0;
    for (;;) {
        int room = bodycap - 1 - total;
        if (room <= 0) { truncated = 1; break; }
        int n = recv(s, body + total, room, 0);
        if (n <= 0) break;
        total += n;
    }
    closesocket(s);
    body[total] = 0;
    (void)truncated;

    if (total < 12) return 0;
    int status = 0;
    if (sscanf(body, "HTTP/%*d.%*d %d", &status) != 1) return 0;
    *out_status = status;

    /* 解析 Location 头 */
    if (location && loccap > 0) {
        const char *p = body;
        while ((p = strstr(p, "Location:")) != NULL) {
            int header_start = (int)(p - body);
            if (header_start > 0 && body[header_start - 1] != '\n') { p += 9; continue; }
            p += 9;
            while (*p == ' ' || *p == '\t') p++;
            const char *end = p;
            while (*end && *end != '\r' && *end != '\n') end++;
            char loc[2048];
            size_t n = (size_t)(end - p);
            if (n >= sizeof(loc)) n = sizeof(loc) - 1;
            memcpy(loc, p, n);
            loc[n] = 0;
            bw_utf8_to_wide(loc, location, loccap);
            break;
        }
    }
    return 1;
}

static int http_probe_online(const BWConfig *cfg, wchar_t *detail, int cap) {
    int timeout_ms = cfg->http_probe_timeout_sec * 1000;
    if (timeout_ms <= 0) timeout_ms = 5000;
    char *body = (char *)malloc(32768);
    if (!body) return 0;
    int status = 0;
    wchar_t location[2048] = { 0 };
    int ok = simple_http_get(cfg->http_probe_url, timeout_ms, body, 32768,
                             &status, location, 2048);
    int online = 0;
    if (!ok) {
        _snwprintf(detail, cap - 1, L"HTTP 探测失败（无法建立连接）");
    } else if (status == 200 && body[0]) {
        if (text_contains_portal(body, cfg->portal_host)) {
            _snwprintf(detail, cap - 1, L"HTTP 200 但返回的是认证门户页面");
        } else {
            online = 1;
            _snwprintf(detail, cap - 1, L"HTTP 200");
        }
    } else if (status >= 300 && status < 400) {
        if (wtext_contains_portal(location, cfg->portal_host) ||
            text_contains_portal(body, cfg->portal_host)) {
            _snwprintf(detail, cap - 1, L"被重定向到认证门户（HTTP %d）", status);
        } else {
            online = 1;
            _snwprintf(detail, cap - 1, L"HTTP %d 跳转到外网", status);
        }
    } else {
        _snwprintf(detail, cap - 1, L"HTTP %d", status);
    }
    detail[cap - 1] = 0;
    free(body);
    return online;
}

static int ping_host(const wchar_t *host, int timeout_ms) {
    char host_utf8[256];
    bw_wide_to_utf8(host, host_utf8, sizeof(host_utf8));
    WSADATA wsa;
    static int wsa_ready = 0;
    if (!wsa_ready) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        wsa_ready = 1;
    }
    ADDRINFOW hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    wchar_t whost[256];
    bw_utf8_to_wide(host_utf8, whost, 256);
    PADDRINFOW result = NULL;
    if (GetAddrInfoW(whost, NULL, &hints, &result) != 0 || !result) return 0;
    struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
    IPAddr ip = addr->sin_addr.s_addr;
    FreeAddrInfoW(result);

    HANDLE icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) return 0;
    unsigned char send_data[32];
    memset(send_data, 'a', sizeof(send_data));
    unsigned char reply[sizeof(ICMP_ECHO_REPLY) + sizeof(send_data) + 8];
    DWORD reply_size = (DWORD)sizeof(reply);
    DWORD n = IcmpSendEcho(icmp, ip, send_data, (WORD)sizeof(send_data),
                           NULL, reply, reply_size, (DWORD)timeout_ms);
    int ok = 0;
    if (n > 0) {
        ICMP_ECHO_REPLY *r = (ICMP_ECHO_REPLY *)reply;
        if (r->Status == IP_SUCCESS) ok = 1;
    }
    IcmpCloseHandle(icmp);
    return ok;
}

int bw_connectivity_check(const BWConfig *cfg, wchar_t *detail, int cap) {
    detail[0] = 0;
    if (cfg->http_probe) {
        if (http_probe_online(cfg, detail, cap)) return 1;
    }
    for (int i = 0; i < cfg->ping_target_count; i++) {
        if (ping_host(cfg->ping_targets[i], cfg->ping_timeout_ms)) {
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