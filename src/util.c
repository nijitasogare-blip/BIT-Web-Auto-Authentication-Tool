/* util.c - 文件/字符串/日志/路径/互斥量等通用工具 */
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <shlobj.h>
#include <shlwapi.h>

wchar_t g_bitweb_config_path[BITWEB_MAX_PATH * 2];

uint64_t bw_now_ms(void) {
    return (uint64_t)GetTickCount64();
}

void bw_sleep_ms(int ms) {
    if (ms > 0) Sleep((DWORD)ms);
}

void bw_format_time(wchar_t *out, int cap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(out, cap, L"%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

int bw_parse_level(const wchar_t *name) {
    if (!name) return BWLOG_INFO;
    if (_wcsicmp(name, L"DEBUG") == 0) return BWLOG_DEBUG;
    if (_wcsicmp(name, L"WARN") == 0 || _wcsicmp(name, L"WARNING") == 0) return BWLOG_WARN;
    if (_wcsicmp(name, L"ERROR") == 0) return BWLOG_ERROR;
    return BWLOG_INFO;
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

void bw_wide_to_utf8(const wchar_t *in, char *out, int cap) {
    if (cap <= 0) return;
    if (!in) { out[0] = 0; return; }
    int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, out, cap, NULL, NULL);
    if (n <= 0) out[0] = 0;
}

void bw_utf8_to_wide(const char *in, wchar_t *out, int cap) {
    if (cap <= 0) return;
    if (!in) { out[0] = 0; return; }
    int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, cap);
    if (n <= 0) out[0] = 0;
}

void bw_url_encode_w(const wchar_t *in, wchar_t *out, int cap) {
    char utf8[4096];
    bw_wide_to_utf8(in, utf8, sizeof(utf8));
    static const wchar_t *hex = L"0123456789ABCDEF";
    int o = 0;
    for (int i = 0; i < (int)sizeof(utf8) && utf8[i] && o < cap - 4; i++) {
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

int bw_wcs_contains(const wchar_t *haystack, const wchar_t *needle) {
    if (!haystack || !needle) return 0;
    return wcsstr(haystack, needle) != NULL;
}

int bw_ascii_only(const char *text) {
    if (!text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p >= 0x80) return 0;
    }
    return 1;
}

void bw_resolve_relative(const wchar_t *base_file, const wchar_t *file,
                         wchar_t *out, int cap) {
    if (!file || !*file) { out[0] = 0; return; }
    if ((file[0] && file[1] == L':') ||
        (file[0] == L'\\' && file[1] == L'\\')) {
        wcsncpy(out, file, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    wchar_t dir[BITWEB_MAX_PATH * 2];
    wcsncpy(dir, base_file, (sizeof(dir) / sizeof(wchar_t)) - 1);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0; else dir[0] = 0;
    swprintf(out, cap, L"%ls\\%ls", dir, file);
    out[cap - 1] = 0;
}

int bw_mkdirs(const wchar_t *path) {
    wchar_t tmp[BITWEB_MAX_PATH * 2];
    size_t len = wcslen(path);
    if (len >= sizeof(tmp) / sizeof(wchar_t) - 2) return 0;
    wcscpy(tmp, path);
    /* 去掉末尾分隔符 */
    while (len > 0 && (tmp[len - 1] == L'\\' || tmp[len - 1] == L'/')) tmp[--len] = 0;
    if (len == 0) return 1;
    for (size_t i = 3; i < len; i++) {
        if (tmp[i] == L'\\' || tmp[i] == L'/') {
            wchar_t saved = tmp[i];
            tmp[i] = 0;
            if (!CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                return 0;
            }
            tmp[i] = saved;
        }
    }
    if (!CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    return 1;
}

static void parent_dir(const wchar_t *path, wchar_t *out, int cap) {
    wcsncpy(out, path, cap - 1);
    out[cap - 1] = 0;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
}

char *bw_read_file_utf8(const wchar_t *path, size_t *out_len) {
    FILE *fp = _wfopen(path, L"rb");
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
    wchar_t dir[BITWEB_MAX_PATH * 2];
    parent_dir(path, dir, (int)(sizeof(dir) / sizeof(wchar_t)));
    bw_mkdirs(dir);
    wchar_t tmp[BITWEB_MAX_PATH * 2];
    swprintf(tmp, sizeof(tmp) / sizeof(wchar_t), L"%ls.tmp", path);
    FILE *fp = _wfopen(tmp, L"wb");
    if (!fp) return 0;
    int ok = (len <= 0) || (fwrite(data, 1, (size_t)len, fp) == (size_t)len);
    fclose(fp);
    if (!ok) { _wremove(tmp); return 0; }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        _wremove(tmp);
        return 0;
    }
    return 1;
}

int bw_get_exe_path(wchar_t *out, int cap) {
    DWORD n = GetModuleFileNameW(NULL, out, cap);
    if (n == 0 || n >= (DWORD)cap) { out[0] = 0; return 0; }
    return 1;
}

int bw_get_exe_dir(wchar_t *out, int cap) {
    if (!bw_get_exe_path(out, cap)) return 0;
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
    return 1;
}

int bw_resolve_config_path(const wchar_t *explicit_path, wchar_t *out, int cap) {
    if (explicit_path && *explicit_path) {
        wcsncpy(out, explicit_path, cap - 1);
        out[cap - 1] = 0;
        return 1;
    }
    wchar_t env[BITWEB_MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"BITWEB_CONFIG", env, (DWORD)(sizeof(env) / sizeof(wchar_t)));
    if (n > 0 && n < (DWORD)(sizeof(env) / sizeof(wchar_t))) {
        wcsncpy(out, env, cap - 1);
        out[cap - 1] = 0;
        return 1;
    }
    wchar_t local[BITWEB_MAX_PATH * 2];
    DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", local, (DWORD)(sizeof(local) / sizeof(wchar_t)));
    if (m > 0) {
        swprintf(out, cap, L"%ls\\BITWebAutoLogin\\config.json", local);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    wchar_t exedir[BITWEB_MAX_PATH * 2];
    if (bw_get_exe_dir(exedir, (int)(sizeof(exedir) / sizeof(wchar_t)))) {
        if (m <= 0) {
            swprintf(out, cap, L"%ls\\config.json", exedir);
            return 1;
        }
    }
    swprintf(out, cap, L"%ls\\BITWebAutoLogin\\config.json", m > 0 ? local : L".");
    return 1;
}

int bw_running_mutex_exists(const wchar_t *name) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}

int bw_signal_daemon_wake(void) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, BITWEB_WAKE_EVENT);
    if (!ev) return 0;
    SetEvent(ev);
    CloseHandle(ev);
    return 1;
}
int bw_ensure_single_instance(const wchar_t *name, HANDLE *mutex_out) {
    HANDLE h = CreateMutexW(NULL, FALSE, name);
    if (!h) return 0; /* 创建失败时按"允许运行"处理 */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return 0;
    }
    if (mutex_out) *mutex_out = h;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 日志                                                                */
/* ------------------------------------------------------------------ */
static void rotate_log(BWLogger *log) {
    if (!log->ready) return;
    fclose(log->fp);
    log->fp = NULL;
    log->ready = 0;
    if (log->backup_count > 0) {
        for (int i = log->backup_count; i >= 1; i--) {
            wchar_t oldp[BITWEB_MAX_PATH * 2], newp[BITWEB_MAX_PATH * 2];
            swprintf(oldp, sizeof(oldp) / sizeof(wchar_t), L"%ls.%d", log->path, i);
            swprintf(newp, sizeof(newp) / sizeof(wchar_t), L"%ls.%d", log->path, i + 1);
            if (i == log->backup_count) _wremove(oldp);
            else MoveFileExW(oldp, newp, MOVEFILE_REPLACE_EXISTING);
        }
        wchar_t first[BITWEB_MAX_PATH * 2];
        swprintf(first, sizeof(first) / sizeof(wchar_t), L"%ls.1", log->path);
        MoveFileExW(log->path, first, MOVEFILE_REPLACE_EXISTING);
    } else {
        _wremove(log->path);
    }
    log->fp = _wfopen(log->path, L"ab");
    log->ready = log->fp != NULL;
}

void bw_logger_init(BWLogger *log, const wchar_t *path, int level,
                    int max_bytes, int backup_count) {
    memset(log, 0, sizeof(*log));
    wcsncpy(log->path, path, (sizeof(log->path) / sizeof(wchar_t)) - 1);
    log->level = level;
    log->max_bytes = max_bytes;
    log->backup_count = backup_count;
    InitializeCriticalSection(&log->lock);
    wchar_t dir[BITWEB_MAX_PATH * 2];
    parent_dir(path, dir, (int)(sizeof(dir) / sizeof(wchar_t)));
    bw_mkdirs(dir);
    log->fp = _wfopen(path, L"ab");
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
    _vsnwprintf(msg, sizeof(msg) / sizeof(wchar_t) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) / sizeof(wchar_t) - 1] = 0;

    wchar_t ts[64];
    bw_format_time(ts, 64);
    wchar_t line[4300];
    swprintf(line, sizeof(line) / sizeof(wchar_t), L"%ls [%ls] %ls\r\n", ts, level_name(level), msg);

    char utf8[12000];
    bw_wide_to_utf8(line, utf8, sizeof(utf8));

    EnterCriticalSection(&log->lock);
    if (log->ready && log->fp) {
        if (log->max_bytes > 0) {
            fseek(log->fp, 0, SEEK_END);
            long sz = ftell(log->fp);
            if (sz > (long)log->max_bytes) rotate_log(log);
        }
        if (log->ready && log->fp) {
            fwrite(utf8, 1, strlen(utf8), log->fp);
            fflush(log->fp);
        }
    }
    LeaveCriticalSection(&log->lock);
}

void bw_logger_close(BWLogger *log) {
    if (!log) return;
    EnterCriticalSection(&log->lock);
    if (log->fp) { fclose(log->fp); log->fp = NULL; }
    log->ready = 0;
    LeaveCriticalSection(&log->lock);
    DeleteCriticalSection(&log->lock);
}

int bw_status_read_log_tail(const wchar_t *path, int lines, wchar_t *out, int cap) {
    out[0] = 0;
    size_t len = 0;
    char *data = bw_read_file_utf8(path, &len);
    if (!data) return 0;
    /* 从后往前找 lines 个换行 */
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
    /* 统一成 CRLF：日志文件里 Python 版写 CRLF、C 版写 CRLF，
       但历史行/外部工具可能只有 LF，EDIT 控件需要 CRLF 才会换行。 */
    int o = 0;
    int prev_cr = 0;
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
