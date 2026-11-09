/* linux/include/bitweb.h - Linux 版共享声明（与 Windows 版结构/名称保持一致） */
#ifndef BITWEB_LINUX_H
#define BITWEB_LINUX_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>
#include <pthread.h>

#ifndef _snwprintf
#define _snwprintf swprintf
#endif
#ifndef _snprintf
#define _snprintf snprintf
#endif

#define BITWEB_VERSION        L"2.1.0-linux"
#define BITWEB_APP_NAME       L"BIT-Web 自动登录"
#define BITWEB_DAEMON_MUTEX   L"bitweb-daemon"
#define BITWEB_GUI_MUTEX      L"bitweb-gui"
#define BITWEB_DAEMON_TASK    L"bitweb"
#define BITWEB_TRAY_TASK      L"bitweb"

#define BITWEB_MAX_SSIDS   8
#define BITWEB_MAX_PINGS   4
#define BITWEB_MAX_PATH    1024
#define BITWEB_HTTP_BODY   (256 * 1024)
#define BITWEB_JSON_TEXT   (64 * 1024)

typedef struct {
    wchar_t username[128];
    char    password_dpapi[4096];   /* Linux 不使用；为配置兼容保留 */
    char    password_plain[512];    /* Linux 密码（或 keyring 引用），文件权限 600 */
    wchar_t portal_host[128];
    wchar_t portal_scheme[16];
    wchar_t portal_ac_id[32];
    int     portal_timeout_sec;

    wchar_t allowed_ssids[BITWEB_MAX_SSIDS][128];
    int     allowed_ssid_count;
    int     ssid_poll_sec;
    int     sleep_confirm_polls;

    int     check_interval_sec;
    wchar_t ping_targets[BITWEB_MAX_PINGS][256];
    int     ping_target_count;
    int     ping_timeout_ms;
    int     http_probe;
    wchar_t http_probe_url[512];
    int     http_probe_timeout_sec;

    int     login_retry_count;
    int     login_retry_delay_sec;
    int     auth_error_backoff_sec;
    int     login_ignore;

    wchar_t log_file[512];
    int     log_max_bytes;
    int     log_backup_count;
    wchar_t log_level[16];
} BWConfig;

typedef struct BWJson BWJson;
BWJson *bw_json_parse(const char *text, wchar_t *err, int errcap);
void    bw_json_free(BWJson *j);
const BWJson *bw_json_get(const BWJson *j, const char *path);
const char   *bw_json_string(const BWJson *j, const char *path);
int           bw_json_int(const BWJson *j, const char *path, int def);
int           bw_json_bool(const BWJson *j, const char *path, int def);
const BWJson *bw_json_array_get(const BWJson *j, const char *path, int index);
int           bw_json_array_count(const BWJson *j, const char *path);

void bw_hmac_md5_hex(const char *token, const char *password, char out_hex[33]);
int  bw_srun_info(const char *username, const char *password, const char *ip,
                  const char *ac_id, const char *token, char *out, int cap);
int  bw_srun_build_login(const char *token, const char *username, const char *password,
                         const char *ac_id, const char *ip,
                         char *hmd5_hex, char *info, int info_cap, char *chksum_hex);

enum { BWLOG_DEBUG = 0, BWLOG_INFO = 1, BWLOG_WARN = 2, BWLOG_ERROR = 3 };
typedef struct {
    FILE            *fp;
    wchar_t          path[BITWEB_MAX_PATH * 2];
    int              level;
    int              max_bytes;
    int              backup_count;
    pthread_mutex_t  lock;
    int              ready;
} BWLogger;

void bw_logger_init(BWLogger *log, const wchar_t *path, int level,
                    int max_bytes, int backup_count);
void bw_log(BWLogger *log, int level, const wchar_t *fmt, ...);
void bw_logger_close(BWLogger *log);

typedef struct {
    int     config_ok;
    wchar_t config_path[BITWEB_MAX_PATH * 2];
    int     password_configured;
    wchar_t username_masked[160];
    wchar_t allowed[512];
    int     wlan_connected;
    int     in_whitelist;
    wchar_t current_ssid[128];
    wchar_t connected_ssids[512];
    wchar_t wlan_state[256];
    int     connectivity_checked;
    int     online;
    wchar_t net_detail[512];
    int     daemon_running;
    int     tray_running;
    wchar_t generated[64];
    wchar_t log_tail[8192];
} BWStatus;

typedef struct {
    int     ok;
    int     status;
    int     body_len;
    char   *body;
    wchar_t location[2048];
    wchar_t final_url[2048];
} BWHttp;

int bw_http_get(const wchar_t *url, int timeout_ms, int follow_redirects, BWHttp *out);
void bw_http_free(BWHttp *out);
BWJson *bw_http_jsonp(const BWHttp *resp, wchar_t *err, int errcap);

typedef struct {
    wchar_t name[128];
    wchar_t ssid[128];
    wchar_t state[64];
    int     connected;
} BWWlan;

int bw_wlan_query(BWWlan *items, int max_items, int *count,
                  wchar_t *err, int errcap);

typedef struct {
    wchar_t url[2048];
    wchar_t host[128];
    wchar_t scheme[16];
    wchar_t ac_id[32];
    wchar_t ip[64];
} BWPortal;

typedef struct {
    int     ok;
    int     already_online;
    int     network_error;
    wchar_t message[512];
    wchar_t client_ip[64];
} BWSrunResult;

int bw_srun_discover(const BWConfig *cfg, BWPortal *portal, wchar_t *err, int errcap);
int bw_srun_login(const BWConfig *cfg, const BWPortal *portal,
                  const char *password, BWSrunResult *result);
int bw_srun_logout(const BWConfig *cfg, const BWPortal *portal, wchar_t *msg, int cap);
int bw_srun_online_info(const BWConfig *cfg,
                        wchar_t *user, int user_cap,
                        wchar_t *ip, int ip_cap,
                        wchar_t *msg, int cap);
int bw_srun_challenge(const BWConfig *cfg, const wchar_t *username,
                      const wchar_t *ip, wchar_t *token, int cap,
                      wchar_t *client_ip, int ip_cap,
                      wchar_t *msg, int msg_cap);

void bw_config_defaults(BWConfig *cfg);
int  bw_config_load(BWConfig *cfg, const wchar_t *path, wchar_t *err, int errcap);
int  bw_config_save(const BWConfig *cfg, const wchar_t *path);
int  bw_config_configured_password(const BWConfig *cfg);
int  bw_config_password(const BWConfig *cfg, char *out, int cap);

int  bw_resolve_config_path(const wchar_t *explicit_path, wchar_t *out, int cap);
int  bw_get_exe_path(wchar_t *out, int cap);
int  bw_get_exe_dir(wchar_t *out, int cap);
int  bw_mkdirs(const wchar_t *path);
char *bw_read_file_utf8(const wchar_t *path, size_t *out_len);
int  bw_write_file_utf8_atomic(const wchar_t *path, const char *data, int len);
uint64_t bw_now_ms(void);
void bw_sleep_ms(int ms);
void bw_format_time(wchar_t *out, int cap);
int  bw_parse_level(const wchar_t *name);
void bw_mask_user(const wchar_t *name, wchar_t *out, int cap);
void bw_resolve_relative(const wchar_t *base_file, const wchar_t *file,
                         wchar_t *out, int cap);
void bw_wide_to_utf8(const wchar_t *in, char *out, int cap);
void bw_utf8_to_wide(const char *in, wchar_t *out, int cap);
void bw_url_encode_w(const wchar_t *in, wchar_t *out, int cap);
void bw_base64_encode(const unsigned char *in, int len, char *out, int cap);
int  bw_base64_decode(const char *in, unsigned char *out, int cap);
int  bw_wcs_contains(const wchar_t *haystack, const wchar_t *needle);
int  bw_ascii_only(const char *text);
int  bw_local_ip_towards(const wchar_t *host, wchar_t *out, int cap);
int  bw_runtime_file(const wchar_t *name, wchar_t *out, int cap);
int  bw_running_mutex_exists(const wchar_t *name);
int  bw_signal_daemon_wake(void);

int bw_connectivity_check(const BWConfig *cfg, wchar_t *detail, int cap);
void bw_status_collect(const BWConfig *cfg, BWStatus *st, int with_network);
void bw_status_format(const BWStatus *st, wchar_t *out, int cap);
int  bw_status_read_log_tail(const wchar_t *path, int lines, wchar_t *out, int cap);
void bw_selftest_build(wchar_t *out, int cap);

int bw_daemon_run(const wchar_t *config_path);
int bw_tui_run(void);
int bw_set_password(void);
int bw_cli_status(int with_network);
int bw_cli_selftest(void);
int bw_cli_crypto_test(void);

extern wchar_t g_bitweb_config_path[BITWEB_MAX_PATH * 2];

#endif
