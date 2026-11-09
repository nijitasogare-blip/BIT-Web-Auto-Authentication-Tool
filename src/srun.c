/* srun.c - 深澜（Srun）门户协议：discover / challenge / login / logout / user_info */
#include <winsock2.h>
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ws2tcpip.h>

static void base_url(const BWConfig *cfg, wchar_t *out, int cap) {
    swprintf(out, cap, L"%ls://%ls", cfg->portal_scheme, cfg->portal_host);
}

static void query_str(const BWJson *j, const char *key, wchar_t *out, int cap) {
    const char *s = bw_json_string(j, key);
    if (s) bw_utf8_to_wide(s, out, cap);
    else out[0] = 0;
}

static int http_json(const BWConfig *cfg, const wchar_t *url, int follow,
                     BWJson **out_json, int *network_error,
                     wchar_t *msg, int cap) {
    BWHttp resp;
    *network_error = 0;
    if (out_json) *out_json = NULL;
    if (!bw_http_get(url, cfg->portal_timeout_sec * 1000, follow, &resp)) {
        *network_error = 1;
        if (msg) { wcsncpy(msg, L"访问门户失败", cap - 1); msg[cap - 1] = 0; }
        return 0;
    }
    if (msg) { wcsncpy(msg, L"", cap - 1); msg[cap - 1] = 0; }
    BWJson *j = bw_http_jsonp(&resp, NULL, 0);
    bw_http_free(&resp);
    if (!j) {
        if (msg) { wcsncpy(msg, L"门户返回格式异常", cap - 1); msg[cap - 1] = 0; }
        return 0;
    }
    if (out_json) *out_json = j;
    return 1;
}

static int try_local_ip_towards(const wchar_t *host, wchar_t *out, int cap) {
    char host_utf8[256];
    bw_wide_to_utf8(host, host_utf8, sizeof(host_utf8));
    WSADATA wsa;
    static int ready = 0;
    if (!ready) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        ready = 1;
    }
    ADDRINFOW hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    PADDRINFOW result = NULL;
    if (GetAddrInfoW(host, L"80", &hints, &result) != 0 || !result) return 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int ok = 0;
    if (s != INVALID_SOCKET) {
        if (connect(s, result->ai_addr, (int)result->ai_addrlen) == 0) {
            struct sockaddr_in local;
            int len = sizeof(local);
            if (getsockname(s, (struct sockaddr *)&local, &len) == 0) {
                char buf[64];
                if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
                    bw_utf8_to_wide(buf, out, cap);
                    ok = 1;
                }
            }
        }
        closesocket(s);
    }
    FreeAddrInfoW(result);
    return ok;
}

static int find_input_value(const char *body, const char *id, wchar_t *out, int cap) {
    const char *p = body;
    while ((p = strstr(p, id)) != NULL) {
        const char *q = p + strlen(id);
        while (*q == ' ' || *q == '\t') q++;
        if (strncmp(q, "value=\"", 7) == 0) {
            q += 7;
            const char *end = strchr(q, '"');
            if (end && end > q) {
                size_t n = (size_t)(end - q);
                if (n < (size_t)cap) {
                    char tmp[512];
                    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
                    memcpy(tmp, q, n);
                    tmp[n] = 0;
                    bw_utf8_to_wide(tmp, out, cap);
                    return 1;
                }
            }
        }
        p += strlen(id);
    }
    return 0;
}

int bw_srun_discover(const BWConfig *cfg, BWPortal *portal, wchar_t *err, int errcap) {
    memset(portal, 0, sizeof(*portal));
    wcsncpy(portal->host, cfg->portal_host, 127);
    wcsncpy(portal->scheme, cfg->portal_scheme, 15);

    wchar_t base[1024];
    base_url(cfg, base, 1024);
    wchar_t url[2048];
    swprintf(url, 2048, L"%ls/", base);

    BWHttp resp;
    if (!bw_http_get(url, cfg->portal_timeout_sec * 1000, 1, &resp)) {
        if (err) wcsncpy(err, L"无法访问认证门户", errcap - 1);
        return 0;
    }
    wcsncpy(portal->url, resp.final_url[0] ? resp.final_url : url, 2047);
    portal->url[2047] = 0;

    /* ac_id：优先 URL query，其次页面隐藏字段，最后配置 */
    const wchar_t *q = wcschr(resp.final_url, L'?');
    if (q) {
        const wchar_t *p = wcsstr(q, L"ac_id=");
        if (p) {
            p += 6;
            int i = 0;
            while (*p && *p != L'&' && i < 31) portal->ac_id[i++] = *p++;
            portal->ac_id[i] = 0;
        }
    }
    if (!portal->ac_id[0] && resp.body) {
        wchar_t value[64];
        if (find_input_value(resp.body, "id=\"ac_id\"", value, 64) ||
            find_input_value(resp.body, "name=\"ac_id\"", value, 64)) {
            wcsncpy(portal->ac_id, value, 31);
            portal->ac_id[31] = 0;
        }
    }
    if (!portal->ac_id[0] && cfg->portal_ac_id[0]) {
        wcsncpy(portal->ac_id, cfg->portal_ac_id, 31);
        portal->ac_id[31] = 0;
    }
    if (!portal->ac_id[0]) {
        if (err) wcsncpy(err, L"无法从门户页面识别 ac_id", errcap - 1);
        bw_http_free(&resp);
        return 0;
    }

    /* 本机 IP：优先页面 user_ip，否则用本地路由探测 */
    if (resp.body) {
        wchar_t value[64];
        if (find_input_value(resp.body, "id=\"user_ip\"", value, 64) ||
            find_input_value(resp.body, "name=\"user_ip\"", value, 64)) {
            if (wcscmp(value, L"0.0.0.0") != 0 && wcscmp(value, L"127.0.0.1") != 0) {
                wcsncpy(portal->ip, value, 63);
            }
        }
    }
    if (!portal->ip[0]) {
        try_local_ip_towards(cfg->portal_host, portal->ip, 64);
    }
    bw_http_free(&resp);
    if (err) err[0] = 0;
    return 1;
}

static int parse_challenge_json(const BWJson *j, BWPortal *portal,
                                const wchar_t *fallback_ip,
                                wchar_t *token, int cap,
                                wchar_t *client_ip, int ip_cap,
                                wchar_t *msg, int msg_cap) {
    if (bw_json_string(j, "error") && strcmp(bw_json_string(j, "error"), "ok") != 0) {
        wchar_t emsg[256];
        query_str(j, "error_msg", emsg, 256);
        if (!emsg[0]) query_str(j, "error", emsg, 256);
        if (msg) { wcsncpy(msg, emsg, msg_cap - 1); msg[msg_cap - 1] = 0; }
        return 0;
    }
    const char *challenge = bw_json_string(j, "challenge");
    if (!challenge || !*challenge) {
        if (msg) { wcsncpy(msg, L"门户未返回 challenge", msg_cap - 1); msg[msg_cap - 1] = 0; }
        return 0;
    }
    bw_utf8_to_wide(challenge, token, cap);
    query_str(j, "client_ip", client_ip, ip_cap);
    if (!client_ip[0] && fallback_ip) {
        wcsncpy(client_ip, fallback_ip, ip_cap - 1);
        client_ip[ip_cap - 1] = 0;
    }
    if (portal && client_ip[0]) {
        wcsncpy(portal->ip, client_ip, 63);
        portal->ip[63] = 0;
    }
    if (msg) msg[0] = 0;
    return 1;
}

int bw_srun_challenge(const BWConfig *cfg, const wchar_t *username,
                      const wchar_t *ip, wchar_t *token, int cap,
                      wchar_t *client_ip, int ip_cap,
                      wchar_t *msg, int msg_cap) {
    wchar_t base[1024], url[4096], enc_user[1024], enc_ip[256];
    base_url(cfg, base, 1024);
    bw_url_encode_w(username, enc_user, 1024);
    bw_url_encode_w(ip, enc_ip, 256);
    swprintf(url, 4096,
             L"%ls/cgi-bin/get_challenge?callback=cb&username=%ls&ip=%ls&_=%llu",
             base, enc_user, enc_ip, (unsigned long long)GetTickCount64());
    BWJson *j = NULL;
    int net_err = 0;
    if (!http_json(cfg, url, 0, &j, &net_err, msg, msg_cap)) {
        if (net_err && msg) { wcsncpy(msg, L"访问门户失败", msg_cap - 1); msg[msg_cap - 1] = 0; }
        return 0;
    }
    BWPortal tmp;
    memset(&tmp, 0, sizeof(tmp));
    int ok = parse_challenge_json(j, &tmp, ip, token, cap, client_ip, ip_cap, msg, msg_cap);
    bw_json_free(j);
    return ok;
}

static int json_already_online(const BWJson *j) {
    const char *keys[] = { "suc_msg", "error_msg", "ploy_msg", "error", "res" };
    char combined[2048];
    combined[0] = 0;
    for (int i = 0; i < 5; i++) {
        const char *s = bw_json_string(j, keys[i]);
        if (s && *s) {
            strncat(combined, s, sizeof(combined) - strlen(combined) - 2);
            strncat(combined, " ", sizeof(combined) - strlen(combined) - 2);
        }
    }
    if (strstr(combined, "ip_already_online")) return 1;
    if (strstr(combined, "already online")) return 1;
    if (strstr(combined, "已在线")) return 1;
    return 0;
}

int bw_srun_login(const BWConfig *cfg, const BWPortal *portal,
                  const char *password, BWSrunResult *result) {
    memset(result, 0, sizeof(*result));
    char username_utf8[256];
    bw_wide_to_utf8(cfg->username, username_utf8, sizeof(username_utf8));

    wchar_t token[256] = { 0 }, client_ip[64] = { 0 }, msg[256] = { 0 };
    if (!bw_srun_challenge(cfg, cfg->username, portal->ip, token, 256,
                           client_ip, 64, msg, 256)) {
        wcsncpy(result->message, msg, 511);
        result->network_error = (msg[0] == 0);
        return 0;
    }

    char ac_id[64], ip[64], hmd5[33], info[8192], chksum[41], token_utf8[256];
    bw_wide_to_utf8(portal->ac_id, ac_id, sizeof(ac_id));
    bw_wide_to_utf8(client_ip[0] ? client_ip : portal->ip, ip, sizeof(ip));
    bw_wide_to_utf8(token, token_utf8, sizeof(token_utf8));
    if (!bw_srun_build_login(token_utf8, username_utf8, password, ac_id, ip,
                             hmd5, info, sizeof(info), chksum)) {
        wcsncpy(result->message, L"构造登录参数失败", 511);
        return 0;
    }
    wchar_t info_w[16384];
    bw_utf8_to_wide(info, info_w, 16384);

    wchar_t base[1024];
    base_url(cfg, base, 1024);
    /* 参数较长，动态分配 URL 缓冲区 */
    int cap = 32768;
    wchar_t *url = (wchar_t *)calloc((size_t)cap, sizeof(wchar_t));
    if (!url) return 0;
    wchar_t enc_user[1024], enc_ac[128], enc_ip[128], enc_info[16384];
    wchar_t hmd5_w[64], chksum_w[64];
    bw_url_encode_w(cfg->username, enc_user, 1024);
    bw_url_encode_w(portal->ac_id, enc_ac, 128);
    bw_url_encode_w(client_ip[0] ? client_ip : portal->ip, enc_ip, 128);
    bw_url_encode_w(info_w, enc_info, 16384);
    bw_utf8_to_wide(hmd5, hmd5_w, 64);
    bw_utf8_to_wide(chksum, chksum_w, 64);
    swprintf(url, cap,
             L"%ls/cgi-bin/srun_portal?callback=cb&action=login&username=%ls"
             L"&password={MD5}%ls&ac_id=%ls&ip=%ls&chksum=%ls&info=%ls"
             L"&n=200&type=1&os=Windows%%2010&name=Windows&double_stack=0&ignore=%d&_=%llu",
             base, enc_user, hmd5_w, enc_ac, enc_ip, chksum_w, enc_info,
             cfg->login_ignore, (unsigned long long)GetTickCount64());

    BWJson *j = NULL;
    int net_err = 0;
    int http_ok = http_json(cfg, url, 0, &j, &net_err, msg, 256);
    free(url);
    if (!http_ok) {
        result->network_error = net_err;
        wcsncpy(result->message, msg[0] ? msg : L"访问门户失败", 511);
        return 0;
    }
    const char *err = bw_json_string(j, "error");
    if (err && strcmp(err, "ok") == 0) {
        result->ok = 1;
        result->already_online = json_already_online(j);
        if (client_ip[0]) wcsncpy(result->client_ip, client_ip, 63);
        bw_json_free(j);
        return 1;
    }
    result->already_online = json_already_online(j);
    wchar_t emsg[512] = { 0 };
    query_str(j, "ploy_msg", emsg, 512);
    if (!emsg[0]) query_str(j, "error_msg", emsg, 512);
    if (!emsg[0]) query_str(j, "suc_msg", emsg, 512);
    if (!emsg[0]) query_str(j, "error", emsg, 512);
    wcsncpy(result->message, emsg, 511);
    bw_json_free(j);
    return 0;
}

int bw_srun_logout(const BWConfig *cfg, const BWPortal *portal,
                   wchar_t *msg, int cap) {
    wchar_t base[1024], url[4096], enc_ac[128], enc_ip[128];
    base_url(cfg, base, 1024);
    bw_url_encode_w(portal->ac_id, enc_ac, 128);
    bw_url_encode_w(portal->ip, enc_ip, 128);
    swprintf(url, 4096,
             L"%ls/cgi-bin/srun_portal?callback=cb&action=logout&ac_id=%ls&ip=%ls&_=%llu",
             base, enc_ac, enc_ip, (unsigned long long)GetTickCount64());
    BWJson *j = NULL;
    int net_err = 0;
    if (!http_json(cfg, url, 0, &j, &net_err, msg, cap)) return 0;
    const char *err = bw_json_string(j, "error");
    int ok = err && strcmp(err, "ok") == 0;
    if (!ok) query_str(j, "error_msg", msg, cap);
    bw_json_free(j);
    return ok;
}

int bw_srun_online_info(const BWConfig *cfg,
                        wchar_t *user, int user_cap,
                        wchar_t *ip, int ip_cap,
                        wchar_t *msg, int cap) {
    wchar_t base[1024], url[2048];
    base_url(cfg, base, 1024);
    swprintf(url, 2048, L"%ls/cgi-bin/rad_user_info?callback=cb&_=%llu",
             base, (unsigned long long)GetTickCount64());
    BWJson *j = NULL;
    int net_err = 0;
    if (!http_json(cfg, url, 0, &j, &net_err, msg, cap)) return 0;
    const char *err = bw_json_string(j, "error");
    int ok = err && strcmp(err, "ok") == 0;
    if (ok) {
        query_str(j, "user_name", user, user_cap);
        query_str(j, "online_ip", ip, ip_cap);
    } else {
        query_str(j, "error_msg", msg, cap);
        if (msg[0] == 0 && err) bw_utf8_to_wide(err, msg, cap);
    }
    bw_json_free(j);
    return ok;
}
