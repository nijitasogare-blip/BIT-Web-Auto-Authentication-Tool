/* http.c - 基于 WinHTTP 的同步 GET（支持手动重定向、超时、JSONP 解析） */
#include "bitweb.h"
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    wchar_t scheme[16];
    wchar_t host[256];
    wchar_t path[2048];
    INTERNET_PORT port;
    int secure;
} UrlParts;

static int crack_url(const wchar_t *url, UrlParts *parts) {
    URL_COMPONENTS uc;
    wchar_t scheme[16] = { 0 }, host[256] = { 0 }, path[2048] = { 0 }, extra[1024] = { 0 };
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszScheme = scheme;
    uc.dwSchemeLength = 15;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1023;
    if (!WinHttpCrackUrl(url, (DWORD)wcslen(url), 0, &uc)) return 0;
    wcsncpy(parts->scheme, scheme, 15);
    wcsncpy(parts->host, host, 255);
    if (extra[0]) {
        _snwprintf(parts->path, 2047, L"%ls%ls", path, extra);
    } else {
        wcsncpy(parts->path, path, 2047);
    }
    parts->path[2047] = 0;
    parts->port = uc.nPort;
    parts->secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return 1;
}

static int resolve_location(const wchar_t *base, const wchar_t *loc,
                            wchar_t *out, int cap) {
    UrlParts p;
    if (!crack_url(base, &p)) return 0;
    if (wcsncmp(loc, L"http://", 7) == 0 || wcsncmp(loc, L"https://", 8) == 0) {
        wcsncpy(out, loc, cap - 1);
        out[cap - 1] = 0;
        return 1;
    }
    if (loc[0] == L'/' && loc[1] == L'/') {
        _snwprintf(out, cap, L"%ls:%ls", p.scheme, loc);
        out[cap - 1] = 0;
        return 1;
    }
    if (loc[0] == L'/') {
        int default_port = p.secure ? 443 : 80;
        if (p.port == 0 || p.port == (INTERNET_PORT)default_port) {
            _snwprintf(out, cap, L"%ls://%ls%ls", p.scheme, p.host, loc);
        } else {
            _snwprintf(out, cap, L"%ls://%ls:%u%ls", p.scheme, p.host,
                       (unsigned)p.port, loc);
        }
        out[cap - 1] = 0;
        return 1;
    }
    /* 相对路径：替换 base path 的最后一段 */
    wchar_t dir[2048];
    wcsncpy(dir, p.path, 2047);
    dir[2047] = 0;
    wchar_t *slash = wcsrchr(dir, L'/');
    if (slash) slash[1] = 0; else wcscpy(dir, L"/");
    int default_port = p.secure ? 443 : 80;
    if (p.port == 0 || p.port == (INTERNET_PORT)default_port) {
        _snwprintf(out, cap, L"%ls://%ls%ls%ls", p.scheme, p.host, dir, loc);
    } else {
        _snwprintf(out, cap, L"%ls://%ls:%u%ls%ls", p.scheme, p.host,
                   (unsigned)p.port, dir, loc);
    }
    out[cap - 1] = 0;
    return 1;
}

static int read_body(HINTERNET request, BWHttp *out) {
    DWORD avail = 0;
    char chunk[8192];
    while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
        DWORD want = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
        DWORD got = 0;
        if (!WinHttpReadData(request, chunk, want, &got)) return 0;
        if (got == 0) break;
        if (out->body_len + (int)got >= BITWEB_HTTP_BODY) {
            got = (DWORD)(BITWEB_HTTP_BODY - out->body_len - 1);
        }
        if (got > 0) {
            memcpy(out->body + out->body_len, chunk, got);
            out->body_len += (int)got;
            out->body[out->body_len] = 0;
        }
    }
    return 1;
}

static int http_once(const wchar_t *url, int timeout_ms, int follow_redirects, BWHttp *out) {
    UrlParts parts;
    if (!crack_url(url, &parts)) return 0;

    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                    L"BITWebAutoLogin/" BITWEB_VERSION,
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET connect = WinHttpConnect(session, parts.host, parts.port, 0);
    if (!connect) { WinHttpCloseHandle(session); return 0; }

    DWORD flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", parts.path, NULL,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return 0;
    }
    if (!follow_redirects) {
        DWORD disable = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    }

    int ok = 0;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL)) {
        DWORD status = 0, slen = sizeof(status);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                                WINHTTP_NO_HEADER_INDEX)) {
            out->status = (int)status;
            wchar_t loc[2048] = { 0 };
            DWORD llen = sizeof(loc);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, loc, &llen,
                                    WINHTTP_NO_HEADER_INDEX)) {
                loc[2047] = 0;
                wcsncpy(out->location, loc, 2047);
                out->location[2047] = 0;
            }
            if (out->body && out->body_len == 0) read_body(request, out);
            ok = 1;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

int bw_http_get(const wchar_t *url, int timeout_ms, int follow_redirects, BWHttp *out) {
    memset(out, 0, sizeof(*out));
    out->body = (char *)calloc(1, BITWEB_HTTP_BODY);
    if (!out->body) return 0;
    wchar_t current[4096];
    wcsncpy(current, url, 4095);
    current[4095] = 0;

    int result = 0;
    for (int hop = 0; hop < 6; hop++) {
        out->status = 0;
        out->body_len = 0;
        out->body[0] = 0;
        out->location[0] = 0;
        wcsncpy(out->final_url, current, 2047);
        out->final_url[2047] = 0;

        if (http_once(current, timeout_ms, follow_redirects, out)) {
            result = 1;
            if (follow_redirects && out->status >= 300 && out->status < 400 &&
                out->location[0]) {
                wchar_t next[4096];
                if (resolve_location(current, out->location, next, 4096)) {
                    wcsncpy(current, next, 4095);
                    current[4095] = 0;
                    continue;
                }
            }
        }
        break;
    }
    out->ok = result;
    return result;
}

void bw_http_free(BWHttp *out) {
    if (!out) return;
    free(out->body);
    out->body = NULL;
    out->body_len = 0;
}

BWJson *bw_http_jsonp(const BWHttp *resp, wchar_t *err, int errcap) {
    if (!resp || !resp->body) return NULL;
    const char *body = resp->body;
    while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') body++;
    if (*body == '{' || *body == '[') {
        return bw_json_parse(body, err, errcap);
    }
    const char *lparen = strchr(body, '(');
    const char *rparen = strrchr(body, ')');
    if (!lparen || !rparen || rparen <= lparen) return NULL;
    size_t n = (size_t)(rparen - lparen - 1);
    char *json = (char *)malloc(n + 1);
    if (!json) return NULL;
    memcpy(json, lparen + 1, n);
    json[n] = 0;
    BWJson *j = bw_json_parse(json, err, errcap);
    free(json);
    return j;
}
