/* linux/src/util.c - POSIX 工具：路径/文件/UTF-8/日志/锁/本地 IP */
#define _GNU_SOURCE
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>
#include <locale.h>

uint64_t bw_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

void bw_sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void bw_format_time(wchar_t *out, int cap) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    swprintf(out, cap, L"%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

static void path_to_utf8(const wchar_t *in, char *out, int cap) {
    bw_wide_to_utf8(in, out, cap);
}

/* ---------------- UTF-8 <-> wchar_t (UTF-32) ---------------- */
void bw_utf8_to_wide(const char *in, wchar_t *out, int cap) {
    if (cap <= 0) return;
    if (!in) { out[0] = 0; return; }
    int o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o < cap - 1) {
        unsigned cp = 0;
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xE0) == 0xC0) {
            cp = (*p++ & 0x1F) << 6;
            if ((*p & 0xC0) != 0x80) break;
            cp |= *p++ & 0x3F;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (*p++ & 0x0F) << 12;
            if ((*p & 0xC0) != 0x80) break;
            cp |= (*p++ & 0x3F) << 6;
            if ((*p & 0xC0) != 0x80) break;
            cp |= *p++ & 0x3F;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = (*p++ & 0x07) << 18;
            for (int i = 0; i < 3; i++) {
                if ((*p & 0xC0) != 0x80) { cp = 0xFFFD; break; }
                cp = (cp << 6) | (*p++ & 0x3F);
            }
        } else {
            cp = 0xFFFD;
            p++;
        }
        out[o++] = (wchar_t)cp;
    }
    out[o] = 0;
}

void bw_wide_to_utf8(const wchar_t *in, char *out, int cap) {
    if (cap <= 0) return;
    if (!in) { out[0] = 0; return; }
    int o = 0;
    for (int i = 0; in[i] && o < cap - 5; i++) {
        unsigned cp = (unsigned)in[i];
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
}

int bw_ascii_only(const char *text) {
    if (!text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p >= 0x80) return 0;
    }
    return 1;
}

int bw_wcs_contains(const wchar_t *haystack, const wchar_t *needle) {
    if (!haystack || !needle) return 0;
    return wcsstr(haystack, needle) != NULL;
}

void bw_mask_user(const wchar_t *name, wchar_t *out, int cap) {
    if (!name || !*name) { wcsncpy(out, L"<未填写>", cap - 1); out[cap - 1] = 0; return; }
    size_t len = wcslen(name);
    if (len <= 4) {
        for (size_t i = 0; i < len && (int)i < cap - 1; i++) out[i] = L'*';
        out[len < (size_t)cap ? len : (size_t)cap - 1] = 0;
        return;
    }
    wcsncpy(out, name, 4);
    out[4] = 0;
    for (size_t i = 4; i < len && (int)i < cap - 1; i++) out[i] = L'*';
    out[len < (size_t)cap ? len : (size_t)cap - 1] = 0;
}

int bw_parse_level(const wchar_t *name) {
    if (!name) return BWLOG_INFO;
    if (!wcscasecmp(name, L"DEBUG")) return BWLOG_DEBUG;
    if (!wcscasecmp(name, L"WARN") || !wcscasecmp(name, L"WARNING")) return BWLOG_WARN;
    if (!wcscasecmp(name, L"ERROR")) return BWLOG_ERROR;
    return BWLOG_INFO;
}

void bw_url_encode_w(const wchar_t *in, wchar_t *out, int cap) {
    char utf8[4096];
    bw_wide_to_utf8(in, utf8, sizeof(utf8));
    static const wchar_t *hex = L"0123456789ABCDEF";
    int o = 0;
    for (int i = 0; utf8[i] && o < cap - 4; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (wchar_t)c;
        } else {
            out[o++] = L'%';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = 0;
}

/* ---------------- base64 ---------------- */
void bw_base64_encode(const unsigned char *in, int len, char *out, int cap) {
    static const char *alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0, i = 0;
    while (i + 2 < len && o + 4 < cap) {
        out[o++] = alpha[in[i] >> 2];
        out[o++] = alpha[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = alpha[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        out[o++] = alpha[in[i + 2] & 63];
        i += 3;
    }
    if (i < len && o + 4 < cap) {
        if (i + 1 == len) {
            out[o++] = alpha[in[i] >> 2];
            out[o++] = alpha[(in[i] & 3) << 4];
            out[o++] = '=';
            out[o++] = '=';
        } else {
            out[o++] = alpha[in[i] >> 2];
            out[o++] = alpha[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            out[o++] = alpha[(in[i + 1] & 15) << 2];
            out[o++] = '=';
        }
    }
    out[o] = 0;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int bw_base64_decode(const char *in, unsigned char *out, int cap) {
    int o = 0, quad[4], qn = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64val(c);
        if (v < 0) return -1;
        quad[qn++] = v;
        if (qn == 4) {
            if (o + 3 > cap) return -1;
            out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
            out[o++] = (unsigned char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
            out[o++] = (unsigned char)(((quad[2] & 3) << 6) | quad[3]);
            qn = 0;
        }
    }
    if (qn == 2) {
        if (o + 1 > cap) return -1;
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
    } else if (qn == 3) {
        if (o + 2 > cap) return -1;
        out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        out[o++] = (unsigned char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
    } else if (qn != 0) {
        return -1;
    }
    return o;
}

/* ---------------- 路径 / 文件 ---------------- */
int bw_mkdirs(const wchar_t *path) {
    char utf8[BITWEB_MAX_PATH * 4];
    path_to_utf8(path, utf8, sizeof(utf8));
    char tmp[BITWEB_MAX_PATH * 4];
    strncpy(tmp, utf8, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    size_t len = strlen(tmp);
    while (len > 0 && tmp[len - 1] == '/') tmp[--len] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return 0;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return 0;
    return 1;
}

static void parent_dir(const wchar_t *path, wchar_t *out, int cap) {
    wcsncpy(out, path, cap - 1);
    out[cap - 1] = 0;
    wchar_t *slash = wcsrchr(out, L'/');
    if (slash) *slash = 0;
}

char *bw_read_file_utf8(const wchar_t *path, size_t *out_len) {
    char utf8[BITWEB_MAX_PATH * 4];
    path_to_utf8(path, utf8, sizeof(utf8));
    FILE *fp = fopen(utf8, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    buf[rd] = 0;
    fclose(fp);
    if (out_len) *out_len = rd;
    return buf;
}

int bw_write_file_utf8_atomic(const wchar_t *path, const char *data, int len) {
    char utf8[BITWEB_MAX_PATH * 4];
    path_to_utf8(path, utf8, sizeof(utf8));
    char dir[BITWEB_MAX_PATH * 4];
    strncpy(dir, utf8, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (slash) {
        wchar_t wdir[BITWEB_MAX_PATH * 2];
        bw_utf8_to_wide(dir, wdir, sizeof(wdir) / sizeof(wchar_t));
        bw_mkdirs(wdir);
    }
    char tmp[BITWEB_MAX_PATH * 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", utf8);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return 0;
    int ok = (len <= 0) || (fwrite(data, 1, (size_t)len, fp) == (size_t)len);
    fclose(fp);
    if (!ok) { unlink(tmp); return 0; }
    if (rename(tmp, utf8) != 0) { unlink(tmp); return 0; }
    return 1;
}

int bw_get_exe_path(wchar_t *out, int cap) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) { out[0] = 0; return 0; }
    buf[n] = 0;
    bw_utf8_to_wide(buf, out, cap);
    return 1;
}

int bw_get_exe_dir(wchar_t *out, int cap) {
    if (!bw_get_exe_path(out, cap)) return 0;
    wchar_t *slash = wcsrchr(out, L'/');
    if (slash) *slash = 0;
    return 1;
}

static void expand_tilde(const wchar_t *in, wchar_t *out, int cap) {
    if (in[0] == L'~' && (in[1] == L'/' || in[1] == 0)) {
        const char *home = getenv("HOME");
        wchar_t whome[BITWEB_MAX_PATH * 2];
        bw_utf8_to_wide(home ? home : "/tmp", whome, sizeof(whome) / sizeof(wchar_t));
        swprintf(out, cap, L"%ls%ls", whome, in + 1);
    } else {
        wcsncpy(out, in, cap - 1);
        out[cap - 1] = 0;
    }
}

int bw_runtime_file(const wchar_t *name, wchar_t *out, int cap) {
    const char *run = getenv("XDG_RUNTIME_DIR");
    wchar_t base[BITWEB_MAX_PATH * 2];
    if (run && *run) {
        bw_utf8_to_wide(run, base, sizeof(base) / sizeof(wchar_t));
    } else {
        swprintf(base, sizeof(base) / sizeof(wchar_t), L"/tmp/bitweb-%u", (unsigned)getuid());
    }
    swprintf(out, cap, L"%ls/bitweb-autologin/%ls", base, name);
    return 1;
}

int bw_resolve_config_path(const wchar_t *explicit_path, wchar_t *out, int cap) {
    if (explicit_path && *explicit_path) {
        expand_tilde(explicit_path, out, cap);
        return 1;
    }
    const char *env = getenv("BITWEB_CONFIG");
    if (env && *env) {
        bw_utf8_to_wide(env, out, cap);
        return 1;
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char buf[BITWEB_MAX_PATH * 2];
    if (xdg && *xdg) snprintf(buf, sizeof(buf), "%s/bitweb-autologin/config.json", xdg);
    else snprintf(buf, sizeof(buf), "%s/.config/bitweb-autologin/config.json",
                  home ? home : "/tmp");
    bw_utf8_to_wide(buf, out, cap);
    return 1;
}

void bw_resolve_relative(const wchar_t *base_file, const wchar_t *file,
                         wchar_t *out, int cap) {
    if (!file || !*file) { out[0] = 0; return; }
    if (file[0] == L'/') {
        wcsncpy(out, file, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    wchar_t dir[BITWEB_MAX_PATH * 2];
    wcsncpy(dir, base_file, sizeof(dir) / sizeof(wchar_t) - 1);
    wchar_t *slash = wcsrchr(dir, L'/');
    if (slash) *slash = 0; else dir[0] = 0;
    swprintf(out, cap, L"%ls/%ls", dir, file);
    out[cap - 1] = 0;
}

int bw_local_ip_towards(const wchar_t *host, wchar_t *out, int cap) {
    char host_utf8[256];
    bw_wide_to_utf8(host, host_utf8, sizeof(host_utf8));
    /* 允许 host 带 :port（测试假门户），getaddrinfo 只接受主机名/IP */
    char *colon = strrchr(host_utf8, ':');
    if (colon) *colon = 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host_utf8, "80", &hints, &res) != 0 || !res) return 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int ok = 0;
    if (s >= 0) {
        if (connect(s, res->ai_addr, res->ai_addrlen) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            if (getsockname(s, (struct sockaddr *)&local, &len) == 0) {
                char buf[64];
                if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
                    bw_utf8_to_wide(buf, out, cap);
                    ok = 1;
                }
            }
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

/* ---------------- 进程/唤醒 ---------------- */
static pid_t read_pid_file(const wchar_t *path) {
    char utf8[BITWEB_MAX_PATH * 4];
    path_to_utf8(path, utf8, sizeof(utf8));
    FILE *fp = fopen(utf8, "r");
    if (!fp) return -1;
    long pid = -1;
    if (fscanf(fp, "%ld", &pid) != 1) pid = -1;
    fclose(fp);
    return (pid_t)pid;
}

int bw_running_mutex_exists(const wchar_t *name) {
    (void)name;
    wchar_t pidfile[BITWEB_MAX_PATH * 2];
    bw_runtime_file(L"daemon.pid", pidfile, sizeof(pidfile) / sizeof(wchar_t));
    pid_t pid = read_pid_file(pidfile);
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0;
}

int bw_signal_daemon_wake(void) {
    wchar_t pidfile[BITWEB_MAX_PATH * 2];
    bw_runtime_file(L"daemon.pid", pidfile, sizeof(pidfile) / sizeof(wchar_t));
    pid_t pid = read_pid_file(pidfile);
    if (pid <= 0) return 0;
    return kill(pid, SIGUSR1) == 0;
}

/* ---------------- 日志 ---------------- */
static void log_rotate(BWLogger *log) {
    if (!log->ready) return;
    fclose(log->fp);
    log->fp = NULL;
    log->ready = 0;
    char base[BITWEB_MAX_PATH * 4];
    path_to_utf8(log->path, base, sizeof(base));
    for (int i = log->backup_count; i >= 1; i--) {
        char oldp[BITWEB_MAX_PATH * 5], newp[BITWEB_MAX_PATH * 5];
        snprintf(oldp, sizeof(oldp), "%s.%d", base, i);
        snprintf(newp, sizeof(newp), "%s.%d", base, i + 1);
        if (i == log->backup_count) unlink(oldp);
        else rename(oldp, newp);
    }
    if (log->backup_count > 0) {
        char first[BITWEB_MAX_PATH * 5];
        snprintf(first, sizeof(first), "%s.1", base);
        rename(base, first);
    } else {
        unlink(base);
    }
    log->fp = fopen(base, "ab");
    log->ready = log->fp != NULL;
}

void bw_logger_init(BWLogger *log, const wchar_t *path, int level,
                    int max_bytes, int backup_count) {
    memset(log, 0, sizeof(*log));
    wcsncpy(log->path, path, sizeof(log->path) / sizeof(wchar_t) - 1);
    log->level = level;
    log->max_bytes = max_bytes;
    log->backup_count = backup_count;
    pthread_mutex_init(&log->lock, NULL);
    wchar_t dir[BITWEB_MAX_PATH * 2];
    parent_dir(path, dir, sizeof(dir) / sizeof(wchar_t));
    bw_mkdirs(dir);
    char utf8[BITWEB_MAX_PATH * 4];
    path_to_utf8(path, utf8, sizeof(utf8));
    log->fp = fopen(utf8, "ab");
    log->ready = log->fp != NULL;
}

static const wchar_t *level_name(int level) {
    switch (level) {
        case BWLOG_DEBUG: return L"DEBUG";
        case BWLOG_WARN: return L"WARN";
        case BWLOG_ERROR: return L"ERROR";
        default: return L"INFO";
    }
}

void bw_log(BWLogger *log, int level, const wchar_t *fmt, ...) {
    if (!log || level < log->level) return;
    wchar_t msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, sizeof(msg) / sizeof(wchar_t) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) / sizeof(wchar_t) - 1] = 0;
    wchar_t ts[64];
    bw_format_time(ts, 64);
    wchar_t line[4300];
    swprintf(line, sizeof(line) / sizeof(wchar_t), L"%ls [%ls] %ls\n",
             ts, level_name(level), msg);
    char utf8[16000];
    bw_wide_to_utf8(line, utf8, sizeof(utf8));

    pthread_mutex_lock(&log->lock);
    if (log->ready && log->fp) {
        if (log->max_bytes > 0) {
            fseek(log->fp, 0, SEEK_END);
            long sz = ftell(log->fp);
            if (sz > (long)log->max_bytes) log_rotate(log);
        }
        if (log->ready && log->fp) {
            fwrite(utf8, 1, strlen(utf8), log->fp);
            fflush(log->fp);
        }
    }
    pthread_mutex_unlock(&log->lock);
}

void bw_logger_close(BWLogger *log) {
    if (!log) return;
    pthread_mutex_lock(&log->lock);
    if (log->fp) { fclose(log->fp); log->fp = NULL; }
    log->ready = 0;
    pthread_mutex_unlock(&log->lock);
    pthread_mutex_destroy(&log->lock);
}

int bw_status_read_log_tail(const wchar_t *path, int lines, wchar_t *out, int cap) {
    out[0] = 0;
    size_t len = 0;
    char *data = bw_read_file_utf8(path, &len);
    if (!data) return 0;
    int found = 0;
    size_t pos = len;
    while (pos > 0) {
        pos--;
        if (data[pos] == '\n') {
            found++;
            if (found > lines) { pos++; break; }
        }
    }
    const char *start = data + pos;
    wchar_t wide[8192];
    bw_utf8_to_wide(start, wide, sizeof(wide) / sizeof(wchar_t));
    int o = 0, prev_cr = 0;
    for (int i = 0; wide[i] && o < cap - 3; i++) {
        wchar_t ch = wide[i];
        if (ch == L'\r') { out[o++] = L'\r'; prev_cr = 1; continue; }
        if (ch == L'\n') {
            if (!prev_cr) out[o++] = L'\r';
            out[o++] = L'\n';
            prev_cr = 0;
            continue;
        }
        out[o++] = ch;
        prev_cr = 0;
    }
    out[o] = 0;
    while (o > 0 && (out[o - 1] == L'\n' || out[o - 1] == L'\r')) out[--o] = 0;
    free(data);
    return 1;
}
