/* linux/src/http.c - POSIX socket HTTP GET（只支持 http，门户与探测都用它） */
#define _GNU_SOURCE
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/select.h>

typedef struct {
    char host[256];
    int  port;
    char path[2048];
} UrlParts;

static int parse_url(const wchar_t *url, UrlParts *p) {
    char utf8[4096];
    bw_wide_to_utf8(url, utf8, sizeof(utf8));
    if (strncmp(utf8, "http://", 7) != 0) return 0;
    const char *s = utf8 + 7;
    const char *slash = strchr(s, '/');
    char authority[512];
    size_t alen = slash ? (size_t)(slash - s) : strlen(s);
    if (alen >= sizeof(authority)) return 0;
    memcpy(authority, s, alen);
    authority[alen] = 0;
    p->port = 80;
    char *colon = strrchr(authority, ':');
    if (colon) {
        *colon = 0;
        p->port = atoi(colon + 1);
        if (p->port <= 0 || p->port > 65535) p->port = 80;
    }
    if (!authority[0]) return 0;
    strncpy(p->host, authority, sizeof(p->host) - 1);
    p->host[sizeof(p->host) - 1] = 0;
    snprintf(p->path, sizeof(p->path), "%s", slash ? slash : "/");
    return 1;
}

static int resolve_url(const wchar_t *base, const wchar_t *loc, wchar_t *out, int cap) {
    char b[4096], l[2048];
    bw_wide_to_utf8(base, b, sizeof(b));
    bw_wide_to_utf8(loc, l, sizeof(l));
    if (strncmp(l, "http://", 7) == 0) {
        bw_utf8_to_wide(l, out, cap);
        return 1;
    }
    char scheme[16] = "http";
    char host[256];
    int port = 80;
    char path[2048] = "/";
    if (sscanf(b, "%15[^:]://%255[^/:]:%d%2047s", scheme, host, &port, path) < 2) {
        /* 不带端口的情况 */
        if (sscanf(b, "%15[^:]://%255[^/]%2047s", scheme, host, path) >= 2) port = 80;
        else return 0;
    }
    char result[4096];
    if (l[0] == '/') {
        if (l[1] == '/') {
            snprintf(result, sizeof(result), "%s:%s", scheme, l);
        } else {
            snprintf(result, sizeof(result), "%s://%s:%d%s", scheme, host, port, l);
        }
    } else {
        char dir[2048];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash) slash[1] = 0; else snprintf(dir, sizeof(dir), "/");
        snprintf(result, sizeof(result), "%s://%s:%d%s%s", scheme, host, port, dir, l);
    }
    bw_utf8_to_wide(result, out, cap);
    return 1;
}

static int connect_timeout(int fd, const struct sockaddr *addr, socklen_t alen, int timeout_ms) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, addr, alen);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = select(fd + 1, NULL, &wf, NULL, &tv);
        if (rc > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            rc = (err == 0) ? 0 : -1;
        } else {
            rc = -1;
        }
    } else if (rc == 0) {
        rc = 0;
    } else {
        rc = -1;
    }
    fcntl(fd, F_SETFL, flags);
    return rc;
}

static char *find_header_value(const char *headers, const char *name) {
    size_t nlen = strlen(name);
    const char *p = headers;
    while (*p) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) line_end = p + strlen(p);
        if ((size_t)(line_end - p) > nlen && strncasecmp(p, name, nlen) == 0 &&
            p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vlen = (size_t)(line_end - v);
            while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n')) vlen--;
            char *out = (char *)malloc(vlen + 1);
            if (!out) return NULL;
            memcpy(out, v, vlen);
            out[vlen] = 0;
            return out;
        }
        p = line_end;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
    }
    return NULL;
}

static int decode_chunked(const char *in, int inlen, char *out, int outcap) {
    int o = 0;
    const char *p = in;
    const char *end = in + inlen;
    while (p < end) {
        char *line_end = NULL;
        long chunk = strtol(p, &line_end, 16);
        if (line_end == p) break;
        p = line_end;
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
        if (chunk <= 0) break;
        if (chunk > end - p) chunk = end - p;
        if (o + chunk > outcap - 1) chunk = outcap - 1 - o;
        memcpy(out + o, p, (size_t)chunk);
        o += (int)chunk;
        p += chunk;
        while (p < end && (*p == '\r' || *p == '\n')) p++;
    }
    out[o] = 0;
    return o;
}

static int http_once(const wchar_t *url, int timeout_ms, BWHttp *out) {
    UrlParts parts;
    if (!parse_url(url, &parts)) return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", parts.port);
    if (getaddrinfo(parts.host, portstr, &hints, &res) != 0 || !res) {
        out->ok = 0;
        return 0;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return 0; }
    int connected = connect_timeout(fd, res->ai_addr, res->ai_addrlen, timeout_ms) == 0;
    freeaddrinfo(res);
    if (!connected) { close(fd); return 0; }
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char request[4096];
    int host_hdr = parts.port == 80 ? 1 : 0;
    if (host_hdr) {
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: BITWebAutoLogin/%ls\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n",
                 parts.path, parts.host, BITWEB_VERSION);
    } else {
        snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: BITWebAutoLogin/%ls\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n",
                 parts.path, parts.host, parts.port, BITWEB_VERSION);
    }
    if (send(fd, request, strlen(request), 0) <= 0) { close(fd); return 0; }

    char *raw = (char *)malloc(BITWEB_HTTP_BODY + 65536);
    if (!raw) { close(fd); return 0; }
    int total = 0, cap = BITWEB_HTTP_BODY + 65536;
    for (;;) {
        int n = (int)recv(fd, raw + total, (size_t)(cap - 1 - total), 0);
        if (n <= 0) break;
        total += n;
        if (total >= cap - 1) break;
    }
    close(fd);
    raw[total] = 0;
    if (total < 12) { free(raw); return 0; }

    int status = 0;
    if (sscanf(raw, "HTTP/%*d.%*d %d", &status) != 1) { free(raw); return 0; }
    out->status = status;
    char *sep = strstr(raw, "\r\n\r\n");
    if (!sep) { free(raw); return 0; }
    *sep = 0;
    char *headers = raw;
    char *body_start = sep + 4;
    int body_len = total - (int)(body_start - raw);
    if (body_len < 0) body_len = 0;

    char *loc = find_header_value(headers, "Location");
    if (loc) {
        bw_utf8_to_wide(loc, out->location, 2048);
        free(loc);
    }
    int chunked = 0;
    char *te = find_header_value(headers, "Transfer-Encoding");
    if (te) {
        if (strcasestr(te, "chunked")) chunked = 1;
        free(te);
    }
    out->body = (char *)malloc((size_t)body_len + 2);
    if (!out->body) { free(raw); return 0; }
    if (chunked) {
        out->body_len = decode_chunked(body_start, body_len, out->body, body_len + 1);
    } else {
        memcpy(out->body, body_start, (size_t)body_len);
        out->body[body_len] = 0;
        out->body_len = body_len;
    }
    free(raw);
    out->ok = 1;
    return 1;
}

int bw_http_get(const wchar_t *url, int timeout_ms, int follow_redirects, BWHttp *out) {
    memset(out, 0, sizeof(*out));
    wchar_t current[4096];
    wcsncpy(current, url, 4095);
    current[4095] = 0;
    out->ok = 0;
    for (int hop = 0; hop < 6; hop++) {
        out->status = 0;
        out->body_len = 0;
        if (out->body) { free(out->body); out->body = NULL; }
        out->location[0] = 0;
        wcsncpy(out->final_url, current, 2047);
        out->final_url[2047] = 0;
        if (!http_once(current, timeout_ms, out)) return 0;
        if (follow_redirects && out->status >= 300 && out->status < 400 && out->location[0]) {
            wchar_t next[4096];
            if (resolve_url(current, out->location, next, 4096)) {
                wcsncpy(current, next, 4095);
                current[4095] = 0;
                continue;
            }
        }
        return 1;
    }
    return 1;
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
    if (*body == '{' || *body == '[') return bw_json_parse(body, err, errcap);
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
