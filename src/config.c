/* config.c - 配置加载/保存、DPAPI 加解密 */
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincrypt.h>

void bw_config_defaults(BWConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    wcscpy(cfg->portal_host, L"10.0.0.55");
    wcscpy(cfg->portal_scheme, L"http");
    cfg->portal_timeout_sec = 6;

    wcscpy(cfg->allowed_ssids[0], L"BIT-Web");
    cfg->allowed_ssid_count = 1;
    cfg->ssid_poll_sec = 15;
    cfg->sleep_confirm_polls = 2;

    cfg->check_interval_sec = 300;
    wcscpy(cfg->ping_targets[0], L"baidu.com");
    wcscpy(cfg->ping_targets[1], L"114.114.114.114");
    cfg->ping_target_count = 2;
    cfg->ping_timeout_ms = 2000;
    cfg->http_probe = 1;
    wcscpy(cfg->http_probe_url, L"http://www.baidu.com/");
    cfg->http_probe_timeout_sec = 5;

    cfg->login_retry_count = 2;
    cfg->login_retry_delay_sec = 5;
    cfg->auth_error_backoff_sec = 1800;
    cfg->login_ignore = 2;

    wcscpy(cfg->log_file, L"logs\\bitweb_monitor.log");
    cfg->log_max_bytes = 1048576;
    cfg->log_backup_count = 3;
    wcscpy(cfg->log_level, L"INFO");
}

static void json_to_wstr(const BWJson *j, const char *path, wchar_t *out, int cap) {
    const char *s = bw_json_string(j, path);
    if (s) bw_utf8_to_wide(s, out, cap);
}

int bw_config_load(BWConfig *cfg, const wchar_t *path, wchar_t *err, int errcap) {
    bw_config_defaults(cfg);
    if (err && errcap > 0) err[0] = 0;
    size_t len = 0;
    char *text = bw_read_file_utf8(path, &len);
    if (!text) {
        if (err) wcsncpy(err, L"配置文件不存在或无法读取", errcap - 1);
        return 0;
    }
    wchar_t jerr[256];
    BWJson *j = bw_json_parse(text, jerr, 256);
    free(text);
    if (!j) {
        if (err) _snwprintf(err, errcap - 1, L"配置文件 JSON 解析失败: %ls", jerr);
        return 0;
    }

    json_to_wstr(j, "username", cfg->username, 128);
    const char *dpapi = bw_json_string(j, "password_dpapi");
    if (dpapi) {
        strncpy(cfg->password_dpapi, dpapi, sizeof(cfg->password_dpapi) - 1);
        cfg->password_dpapi[sizeof(cfg->password_dpapi) - 1] = 0;
    }
    const char *plain = bw_json_string(j, "password");
    if (plain) {
        strncpy(cfg->password_plain, plain, sizeof(cfg->password_plain) - 1);
        cfg->password_plain[sizeof(cfg->password_plain) - 1] = 0;
    }

    json_to_wstr(j, "portal.host", cfg->portal_host, 128);
    json_to_wstr(j, "portal.scheme", cfg->portal_scheme, 16);
    json_to_wstr(j, "portal.ac_id", cfg->portal_ac_id, 32);
    cfg->portal_timeout_sec = bw_json_int(j, "portal.timeout_sec", cfg->portal_timeout_sec);

    int n = bw_json_array_count(j, "network.allowed_ssids");
    if (n > 0) {
        cfg->allowed_ssid_count = 0;
        for (int i = 0; i < n && i < BITWEB_MAX_SSIDS; i++) {
            const BWJson *item = bw_json_array_get(j, "network.allowed_ssids", i);
            if (item) {
                const char *val = bw_json_string(item, "");
                if (val && *val) {
                    bw_utf8_to_wide(val, cfg->allowed_ssids[cfg->allowed_ssid_count], 128);
                    cfg->allowed_ssid_count++;
                }
            }
        }
        if (cfg->allowed_ssid_count == 0) {
            wcscpy(cfg->allowed_ssids[0], L"BIT-Web");
            cfg->allowed_ssid_count = 1;
        }
    }
    cfg->ssid_poll_sec = bw_json_int(j, "network.ssid_poll_sec", cfg->ssid_poll_sec);
    cfg->sleep_confirm_polls = bw_json_int(j, "network.sleep_confirm_polls", cfg->sleep_confirm_polls);

    cfg->check_interval_sec = bw_json_int(j, "connectivity.check_interval_sec", cfg->check_interval_sec);
    n = bw_json_array_count(j, "connectivity.ping_targets");
    if (n > 0) {
        cfg->ping_target_count = 0;
        for (int i = 0; i < n && i < BITWEB_MAX_PINGS; i++) {
            const BWJson *item = bw_json_array_get(j, "connectivity.ping_targets", i);
            const char *val = item ? bw_json_string(item, "") : NULL;
            if (val && *val) {
                bw_utf8_to_wide(val, cfg->ping_targets[cfg->ping_target_count], 256);
                cfg->ping_target_count++;
            }
        }
        if (cfg->ping_target_count == 0) {
            wcscpy(cfg->ping_targets[0], L"baidu.com");
            cfg->ping_target_count = 1;
        }
    }
    cfg->ping_timeout_ms = bw_json_int(j, "connectivity.ping_timeout_ms", cfg->ping_timeout_ms);
    cfg->http_probe = bw_json_bool(j, "connectivity.http_probe", cfg->http_probe);
    json_to_wstr(j, "connectivity.http_probe_url", cfg->http_probe_url, 512);
    cfg->http_probe_timeout_sec = bw_json_int(j, "connectivity.http_probe_timeout_sec",
                                              cfg->http_probe_timeout_sec);

    cfg->login_retry_count = bw_json_int(j, "login.retry_count", cfg->login_retry_count);
    cfg->login_retry_delay_sec = bw_json_int(j, "login.retry_delay_sec", cfg->login_retry_delay_sec);
    cfg->auth_error_backoff_sec = bw_json_int(j, "login.auth_error_backoff_sec",
                                              cfg->auth_error_backoff_sec);
    cfg->login_ignore = bw_json_int(j, "login.ignore", cfg->login_ignore);

    json_to_wstr(j, "logging.file", cfg->log_file, 512);
    cfg->log_max_bytes = bw_json_int(j, "logging.max_bytes", cfg->log_max_bytes);
    cfg->log_backup_count = bw_json_int(j, "logging.backup_count", cfg->log_backup_count);
    json_to_wstr(j, "logging.level", cfg->log_level, 16);
    bw_json_free(j);

    /* 基本约束 */
    if (cfg->ssid_poll_sec < 5) cfg->ssid_poll_sec = 5;
    if (cfg->check_interval_sec < 30) cfg->check_interval_sec = 30;
    if (cfg->sleep_confirm_polls < 1) cfg->sleep_confirm_polls = 1;
    if (cfg->login_retry_count < 0) cfg->login_retry_count = 0;
    if (cfg->login_retry_count > 10) cfg->login_retry_count = 10;
    if (cfg->allowed_ssid_count <= 0) {
        wcscpy(cfg->allowed_ssids[0], L"BIT-Web");
        cfg->allowed_ssid_count = 1;
    }
    return 1;
}

int bw_config_configured_password(const BWConfig *cfg) {
    if (cfg->password_dpapi[0]) return 1;
    return cfg->password_plain[0] != 0;
}

/* ---------------------------- DPAPI -------------------------------- */
static const unsigned char DPAPI_ENTROPY[] = "BITWebAutoLogin/v1";

static int dpapi_crypt(int protect, const unsigned char *in, int inlen,
                       unsigned char *out, int cap) {
    DATA_BLOB din, dent, dout;
    din.cbData = (DWORD)inlen;
    din.pbData = (BYTE *)in;
    dent.cbData = (DWORD)(sizeof(DPAPI_ENTROPY) - 1);
    dent.pbData = (BYTE *)DPAPI_ENTROPY;
    memset(&dout, 0, sizeof(dout));
    BOOL ok;
    if (protect) {
        ok = CryptProtectData(&din, L"BITWebAutoLogin", &dent, NULL, NULL, 0, &dout);
    } else {
        ok = CryptUnprotectData(&din, NULL, &dent, NULL, NULL, 0, &dout);
    }
    if (!ok) return -1;
    int n = (int)dout.cbData;
    if (n > cap) { LocalFree(dout.pbData); return -1; }
    memcpy(out, dout.pbData, (size_t)n);
    LocalFree(dout.pbData);
    return n;
}

int bw_dpapi_protect(const unsigned char *in, int len, char *out_b64, int cap) {
    unsigned char encrypted[4096];
    int n = dpapi_crypt(1, in, len, encrypted, sizeof(encrypted));
    if (n < 0) return 0;
    bw_base64_encode(encrypted, n, out_b64, cap);
    return out_b64[0] != 0;
}

int bw_dpapi_unprotect(const char *in_b64, unsigned char *out, int cap) {
    unsigned char encrypted[4096];
    int n = bw_base64_decode(in_b64, encrypted, sizeof(encrypted));
    if (n <= 0) return -1;
    return dpapi_crypt(0, encrypted, n, out, cap);
}

int bw_config_password(const BWConfig *cfg, char *out, int cap) {
    if (cap <= 0) return 0;
    out[0] = 0;
    if (cfg->password_dpapi[0]) {
        unsigned char plain[512];
        int n = bw_dpapi_unprotect(cfg->password_dpapi, plain, sizeof(plain) - 1);
        if (n <= 0) return 0;
        plain[n] = 0;
        strncpy(out, (char *)plain, cap - 1);
        out[cap - 1] = 0;
        return 1;
    }
    if (cfg->password_plain[0]) {
        strncpy(out, cfg->password_plain, cap - 1);
        out[cap - 1] = 0;
        return 1;
    }
    return 0;
}

/* ---------------------------- 保存 JSON ---------------------------- */
typedef struct { char *p; int len, cap; } SB;

static int sb_init(SB *sb, int cap) {
    sb->p = (char *)malloc(cap);
    if (!sb->p) return 0;
    sb->len = 0;
    sb->cap = cap;
    sb->p[0] = 0;
    return 1;
}

static void sb_add(SB *sb, const char *text) {
    int n = (int)strlen(text);
    if (sb->len + n >= sb->cap) n = sb->cap - sb->len - 1;
    if (n <= 0) return;
    memcpy(sb->p + sb->len, text, (size_t)n);
    sb->len += n;
    sb->p[sb->len] = 0;
}

static void sb_addf(SB *sb, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(tmp, sizeof(tmp) - 1, fmt, ap);
    va_end(ap);
    tmp[sizeof(tmp) - 1] = 0;
    sb_add(sb, tmp);
}

static void sb_json_str(SB *sb, const wchar_t *value) {
    char utf8[4096];
    bw_wide_to_utf8(value, utf8, sizeof(utf8));
    sb_add(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)utf8; *p; p++) {
        if (*p == '"' || *p == '\\') { sb_add(sb, "\\"); sb_add(sb, (char *)p); }
        else if (*p < 0x20) {
            char esc[8];
            _snprintf(esc, sizeof(esc), "\\u%04x", *p);
            sb_add(sb, esc);
        } else {
            char one[2] = { (char)*p, 0 };
            sb_add(sb, one);
        }
    }
    sb_add(sb, "\"");
}

int bw_config_save(const BWConfig *cfg, const wchar_t *path) {
    SB sb;
    if (!sb_init(&sb, 32768)) return 0;
    sb_add(&sb, "{\n  \"username\": ");
    sb_json_str(&sb, cfg->username);
    sb_add(&sb, ",\n  \"password_dpapi\": ");
    sb_add(&sb, "\"");
    {
        char esc[8192];
        int o = 0;
        for (const char *p = cfg->password_dpapi; *p && o < (int)sizeof(esc) - 2; p++) {
            if (*p == '"' || *p == '\\') esc[o++] = '\\';
            esc[o++] = *p;
        }
        esc[o] = 0;
        sb_add(&sb, esc);
    }
    sb_add(&sb, "\",\n  \"password\": \"\",\n");
    sb_add(&sb, "  \"portal\": {\n    \"host\": ");
    sb_json_str(&sb, cfg->portal_host);
    sb_add(&sb, ",\n    \"scheme\": ");
    sb_json_str(&sb, cfg->portal_scheme);
    sb_add(&sb, ",\n    \"ac_id\": ");
    sb_json_str(&sb, cfg->portal_ac_id);
    sb_addf(&sb, ",\n    \"timeout_sec\": %d\n  },\n", cfg->portal_timeout_sec);

    sb_add(&sb, "  \"network\": {\n    \"allowed_ssids\": [");
    for (int i = 0; i < cfg->allowed_ssid_count; i++) {
        if (i) sb_add(&sb, ", ");
        sb_json_str(&sb, cfg->allowed_ssids[i]);
    }
    sb_addf(&sb, "],\n    \"ssid_poll_sec\": %d,\n    \"sleep_confirm_polls\": %d\n  },\n",
            cfg->ssid_poll_sec, cfg->sleep_confirm_polls);

    sb_addf(&sb, "  \"connectivity\": {\n    \"check_interval_sec\": %d,\n    \"ping_targets\": [",
            cfg->check_interval_sec);
    for (int i = 0; i < cfg->ping_target_count; i++) {
        if (i) sb_add(&sb, ", ");
        sb_json_str(&sb, cfg->ping_targets[i]);
    }
    sb_addf(&sb, "],\n    \"ping_timeout_ms\": %d,\n    \"http_probe\": %s,\n    \"http_probe_url\": ",
            cfg->ping_timeout_ms, cfg->http_probe ? "true" : "false");
    sb_json_str(&sb, cfg->http_probe_url);
    sb_addf(&sb, ",\n    \"http_probe_timeout_sec\": %d\n  },\n", cfg->http_probe_timeout_sec);

    sb_addf(&sb, "  \"login\": {\n    \"retry_count\": %d,\n    \"retry_delay_sec\": %d,\n"
                 "    \"auth_error_backoff_sec\": %d,\n    \"ignore\": %d\n  },\n",
            cfg->login_retry_count, cfg->login_retry_delay_sec,
            cfg->auth_error_backoff_sec, cfg->login_ignore);

    sb_add(&sb, "  \"logging\": {\n    \"level\": ");
    sb_json_str(&sb, cfg->log_level);
    sb_add(&sb, ",\n    \"file\": ");
    sb_json_str(&sb, cfg->log_file);
    sb_addf(&sb, ",\n    \"max_bytes\": %d,\n    \"backup_count\": %d\n  }\n}\n",
            cfg->log_max_bytes, cfg->log_backup_count);

    int ok = bw_write_file_utf8_atomic(path, sb.p, sb.len);
    free(sb.p);
    return ok;
}
