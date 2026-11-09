/* linux/src/daemon.c - Linux 守护进程：SSID 状态机 + 只读探测 + 自动认证 */
#define _GNU_SOURCE
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <poll.h>

typedef struct {
    BWConfig cfg;
    wchar_t  config_path[BITWEB_MAX_PATH * 2];
    BWLogger log;
    int      mode;
    int      sleep_pending;
    uint64_t next_check;
    uint64_t next_idle_check;
    uint64_t auth_fail_until;
    uint64_t config_error_until;
    uint64_t last_mtime;
} Daemon;

static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_wake_requested = 0;

static void on_stop(int sig) { (void)sig; g_stop_requested = 1; }
static void on_wake(int sig) { (void)sig; g_wake_requested = 1; }

static uint64_t file_mtime(const wchar_t *path) {
    char utf8[BITWEB_MAX_PATH * 4];
    bw_wide_to_utf8(path, utf8, sizeof(utf8));
    struct stat st;
    if (stat(utf8, &st) != 0) return 0;
    return (uint64_t)st.st_mtim.tv_sec * 1000 + (uint64_t)(st.st_mtim.tv_nsec / 1000000);
}

static int looks_auth_error(const wchar_t *message) {
    static const wchar_t *keys[] = {
        L"密码", L"password", L"用户名", L"账号", L"账户", L"不存在",
        L"冻结", L"锁定", L"验证码", L"欠费", L"余额", L"过期", L"停机",
        L"invalid", L"not found"
    };
    if (!message) return 0;
    wchar_t lower[1024];
    wcsncpy(lower, message, 1023);
    lower[1023] = 0;
    for (int i = 0; lower[i]; i++) {
        if (lower[i] >= L'A' && lower[i] <= L'Z') lower[i] += 32;
    }
    for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
        if (wcsstr(lower, keys[i])) return 1;
    }
    return 0;
}

static int ssid_allowed(const BWConfig *cfg, const wchar_t *ssid) {
    for (int i = 0; i < cfg->allowed_ssid_count; i++) {
        if (wcscmp(cfg->allowed_ssids[i], ssid) == 0) return 1;
    }
    return 0;
}

static int active_ssid(const BWConfig *cfg, wchar_t *ssid, int cap,
                       wchar_t *connected, int ccap, wchar_t *err, int ecap) {
    BWWlan items[16];
    int count = 0;
    ssid[0] = 0;
    if (connected && ccap > 0) connected[0] = 0;
    if (!bw_wlan_query(items, 16, &count, err, ecap)) return 0;
    for (int i = 0; i < count; i++) {
        if (!items[i].connected || !items[i].ssid[0]) continue;
        if (connected && ccap > 0) {
            if (connected[0]) wcsncat(connected, L"、", ccap - wcslen(connected) - 1);
            wcsncat(connected, items[i].ssid, ccap - wcslen(connected) - 1);
        }
        if (!ssid[0] && ssid_allowed(cfg, items[i].ssid)) {
            wcsncpy(ssid, items[i].ssid, cap - 1);
            ssid[cap - 1] = 0;
        }
    }
    return ssid[0] != 0;
}

static int wait_online(Daemon *d) {
    for (int i = 0; i < 2; i++) {
        bw_sleep_ms(i == 0 ? 1500 : 3000);
        wchar_t detail[512];
        if (bw_connectivity_check(&d->cfg, detail, 512)) {
            bw_log(&d->log, BWLOG_INFO, L"登录成功，外网恢复：%ls", detail);
            return 1;
        }
    }
    bw_log(&d->log, BWLOG_WARN, L"认证接口返回成功，但外网仍不通");
    return 0;
}

static int login_once(Daemon *d) {
    BWPortal portal;
    wchar_t err[512];
    if (!bw_srun_discover(&d->cfg, &portal, err, 512)) {
        bw_log(&d->log, BWLOG_WARN, L"无法获取门户参数：%ls", err);
        return 0;
    }
    char password[512];
    if (!bw_config_password(&d->cfg, password, sizeof(password))) {
        bw_log(&d->log, BWLOG_ERROR, L"尚未设置密码，请运行 set-password");
        return 3;
    }
    char username_utf8[256];
    bw_wide_to_utf8(d->cfg.username, username_utf8, sizeof(username_utf8));
    if (!bw_ascii_only(username_utf8) || !bw_ascii_only(password)) {
        bw_log(&d->log, BWLOG_ERROR, L"学号/密码包含非 ASCII 字符，当前协议实现不支持");
        return 2;
    }
    BWSrunResult r;
    if (bw_srun_login(&d->cfg, &portal, password, &r) && r.ok && !r.already_online) {
        wchar_t mask[160];
        bw_mask_user(d->cfg.username, mask, 160);
        bw_log(&d->log, BWLOG_INFO, L"认证接口返回成功（user=%ls, ip=%ls）", mask, portal.ip);
        return 1;
    }
    if (r.ok && r.already_online) {
        bw_log(&d->log, BWLOG_WARN,
               L"门户返回“该 IP 已在线”（ip=%ls），说明 AC 上仍有残留会话", portal.ip);
        return 4;
    }
    bw_log(&d->log, BWLOG_WARN, L"登录失败：%ls", r.message[0] ? r.message : L"未知错误");
    if (looks_auth_error(r.message)) return 2;
    return 0;
}

static int logout_residual(Daemon *d) {
    BWPortal portal;
    wchar_t err[512];
    if (!bw_srun_discover(&d->cfg, &portal, err, 512)) {
        bw_log(&d->log, BWLOG_WARN, L"注销残留会话失败：%ls", err);
        return 0;
    }
    wchar_t msg[512] = { 0 };
    if (bw_srun_logout(&d->cfg, &portal, msg, 512)) {
        bw_log(&d->log, BWLOG_INFO, L"已注销残留会话（ip=%ls），稍后重新认证", portal.ip);
        return 1;
    }
    bw_log(&d->log, BWLOG_WARN, L"注销残留会话失败：%ls", msg);
    return 0;
}

static void set_auth_cooldown(Daemon *d) {
    d->auth_fail_until = bw_now_ms() + (uint64_t)d->cfg.auth_error_backoff_sec * 1000;
    bw_log(&d->log, BWLOG_ERROR,
           L"疑似账号/密码问题，%d 分钟内不再自动重试；请核对后重新运行 set-password",
           d->cfg.auth_error_backoff_sec / 60);
}

static int login_flow(Daemon *d) {
    int attempts = d->cfg.login_retry_count + 1;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        bw_log(&d->log, BWLOG_INFO, L"尝试自动登录（第 %d/%d 次）", attempt, attempts);
        int rc = login_once(d);
        if (rc == 1) {
            if (wait_online(d)) return 1;
        } else if (rc == 3) {
            d->config_error_until = bw_now_ms() + 600000;
            bw_log(&d->log, BWLOG_WARN, L"未配置密码：10 分钟内不再重复尝试");
            return 0;
        } else if (rc == 2) {
            set_auth_cooldown(d);
            return 0;
        } else if (rc == 4) {
            if (logout_residual(d)) {
                int rc2 = login_once(d);
                if (rc2 == 1 && wait_online(d)) return 1;
                if (rc2 == 2) { set_auth_cooldown(d); return 0; }
            }
            bw_log(&d->log, BWLOG_ERROR,
                   L"残留会话清理后仍无法恢复外网：10 分钟内不再重试"
                   L"（可手动打开 http://%ls 退出登录）", d->cfg.portal_host);
            d->auth_fail_until = bw_now_ms() + 600000;
            return 0;
        }
        if (attempt < attempts) bw_sleep_ms(d->cfg.login_retry_delay_sec * 1000);
    }
    return 0;
}

static void maybe_reload(Daemon *d) {
    uint64_t mt = file_mtime(d->config_path);
    if (mt == 0 || mt == d->last_mtime) return;
    BWConfig old = d->cfg;
    wchar_t err[256];
    if (!bw_config_load(&d->cfg, d->config_path, err, 256)) {
        bw_log(&d->log, BWLOG_WARN, L"配置文件已改动但读取失败，继续使用旧配置：%ls", err);
        return;
    }
    d->last_mtime = mt;
    int cred_changed = wcscmp(old.username, d->cfg.username) != 0 ||
                       strcmp(old.password_dpapi, d->cfg.password_dpapi) != 0 ||
                       strcmp(old.password_plain, d->cfg.password_plain) != 0;
    if (cred_changed) {
        d->auth_fail_until = 0;
        d->config_error_until = 0;
        bw_log(&d->log, BWLOG_INFO, L"检测到用户名/密码更新，已重新加载并重置登录冷却");
    } else {
        bw_log(&d->log, BWLOG_INFO, L"检测到配置文件更新，已重新加载");
    }
}

static void connectivity_cycle(Daemon *d) {
    wchar_t detail[512];
    int online = bw_connectivity_check(&d->cfg, detail, 512);
    if (online) {
        bw_log(&d->log, BWLOG_INFO, L"连通性正常：%ls", detail);
        d->auth_fail_until = 0;
        d->config_error_until = 0;
        d->next_check = bw_now_ms() + (uint64_t)d->cfg.check_interval_sec * 1000;
        return;
    }
    bw_log(&d->log, BWLOG_WARN, L"外网不通：%ls", detail);
    uint64_t now = bw_now_ms();
    if (now < d->auth_fail_until) {
        bw_log(&d->log, BWLOG_WARN, L"登录冷却中（还剩 %llu 秒），本轮不重试",
               (unsigned long long)((d->auth_fail_until - now) / 1000));
        d->next_check = now + (uint64_t)d->cfg.check_interval_sec * 1000;
        return;
    }
    if (now < d->config_error_until) {
        bw_log(&d->log, BWLOG_WARN,
               L"未配置密码/配置异常冷却中（还剩 %llu 秒），请运行 set-password",
               (unsigned long long)((d->config_error_until - now) / 1000));
        d->next_check = now + (uint64_t)d->cfg.check_interval_sec * 1000;
        return;
    }
    int recovered = login_flow(d);
    d->next_check = bw_now_ms() + (uint64_t)d->cfg.check_interval_sec * 1000;
    if (recovered) {
        bw_log(&d->log, BWLOG_INFO, L"网络已恢复，%d 秒后再次检查", d->cfg.check_interval_sec);
    } else {
        bw_log(&d->log, BWLOG_WARN, L"%d 秒后再次检查并尝试恢复", d->cfg.check_interval_sec);
    }
}

static void idle_probe(Daemon *d) {
    wchar_t detail[512];
    int online = bw_connectivity_check(&d->cfg, detail, 512);
    bw_log(&d->log, online ? BWLOG_INFO : BWLOG_WARN,
           L"非白名单网络连通性（仅检测，不认证）：%ls（%ls）",
           online ? L"正常" : L"不通", detail);
    d->next_idle_check = bw_now_ms() + (uint64_t)d->cfg.check_interval_sec * 1000;
}

static void daemon_tick(Daemon *d, int force) {
    maybe_reload(d);
    wchar_t err[256] = { 0 }, ssid[128] = { 0 }, connected[512] = { 0 };
    int active = active_ssid(&d->cfg, ssid, 128, connected, 512, err, 256);
    if (!active && err[0]) bw_log(&d->log, BWLOG_DEBUG, L"WLAN 状态读取失败：%ls", err);
    uint64_t now = bw_now_ms();
    if (active) {
        d->sleep_pending = 0;
        if (d->mode != 1) {
            d->mode = 1;
            d->auth_fail_until = 0;
            d->config_error_until = 0;
            d->next_check = now;
            bw_log(&d->log, BWLOG_INFO,
                   L"检测到白名单 SSID「%ls」，开始监听（下次连通性检查：立即）", ssid);
        }
        if (force) d->next_check = now;
    } else {
        if (d->mode == 1) {
            d->sleep_pending++;
            if (d->sleep_pending >= d->cfg.sleep_confirm_polls) {
                d->mode = 0;
                d->next_check = 0;
                d->next_idle_check = now;
                bw_log(&d->log, BWLOG_INFO,
                       L"当前无白名单 SSID（已连接：%ls），进入休眠：仅做只读连通性检测，不访问门户、不认证",
                       connected[0] ? connected : L"无");
            }
        }
        if (d->mode == 0) {
            if (force || now >= d->next_idle_check) idle_probe(d);
            return;
        }
    }
    if (d->sleep_pending) return;
    if (now < d->next_check) return;
    connectivity_cycle(d);
}

static void parent_path(const wchar_t *path, wchar_t *out, int cap) {
    wcsncpy(out, path, cap - 1);
    out[cap - 1] = 0;
    wchar_t *slash = wcsrchr(out, L'/');
    if (slash) *slash = 0;
}

static int acquire_lock(wchar_t *lockpath, wchar_t *pidpath, int *lockfd) {
    bw_runtime_file(L"daemon.lock", lockpath, BITWEB_MAX_PATH * 2);
    bw_runtime_file(L"daemon.pid", pidpath, BITWEB_MAX_PATH * 2);
    wchar_t dir[BITWEB_MAX_PATH * 2];
    parent_path(lockpath, dir, BITWEB_MAX_PATH * 2);
    bw_mkdirs(dir);
    char lock_utf8[BITWEB_MAX_PATH * 4];
    bw_wide_to_utf8(lockpath, lock_utf8, sizeof(lock_utf8));
    int fd = open(lock_utf8, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return 0;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return 0; }
    char pid_utf8[BITWEB_MAX_PATH * 4];
    bw_wide_to_utf8(pidpath, pid_utf8, sizeof(pid_utf8));
    FILE *fp = fopen(pid_utf8, "w");
    if (fp) {
        fprintf(fp, "%d\n", (int)getpid());
        fclose(fp);
    }
    *lockfd = fd;
    return 1;
}

int bw_daemon_run(const wchar_t *config_path) {
    Daemon d;
    memset(&d, 0, sizeof(d));
    wcsncpy(d.config_path, config_path, sizeof(d.config_path) / sizeof(wchar_t) - 1);
    wchar_t err[256] = { 0 };
    int loaded = bw_config_load(&d.cfg, d.config_path, err, 256);
    if (!loaded) bw_config_defaults(&d.cfg);

    wchar_t log_path[BITWEB_MAX_PATH * 2];
    bw_resolve_relative(d.config_path, d.cfg.log_file, log_path,
                        sizeof(log_path) / sizeof(wchar_t));
    bw_logger_init(&d.log, log_path, bw_parse_level(d.cfg.log_level),
                   d.cfg.log_max_bytes, d.cfg.log_backup_count);
    d.last_mtime = file_mtime(d.config_path);
    if (!loaded) bw_log(&d.log, BWLOG_ERROR, L"配置文件读取失败：%ls", err);
    bw_log(&d.log, BWLOG_INFO, L"守护进程启动（pid=%d，配置=%ls）", (int)getpid(), d.config_path);

    wchar_t lockpath[BITWEB_MAX_PATH * 2], pidpath[BITWEB_MAX_PATH * 2];
    int lockfd = -1;
    if (!acquire_lock(lockpath, pidpath, &lockfd)) {
        bw_log(&d.log, BWLOG_WARN, L"已有实例在运行，本实例退出");
        bw_logger_close(&d.log);
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_wake;
    sa.sa_flags = 0;   /* 让 poll 被 SIGUSR1 中断，立即处理唤醒 */
    sigaction(SIGUSR1, &sa, NULL);

    int poll_ms = d.cfg.ssid_poll_sec * 1000;
    if (poll_ms < 5000) poll_ms = 5000;
    daemon_tick(&d, 0);

    while (!g_stop_requested) {
        if (g_wake_requested) {
            g_wake_requested = 0;
            bw_log(&d.log, BWLOG_INFO, L"收到立即检查请求（手动刷新/终端重连）");
            daemon_tick(&d, 1);
            continue;
        }
        int rc = poll(NULL, 0, poll_ms);
        if (rc < 0 && errno != EINTR) bw_sleep_ms(poll_ms);
        if (!g_stop_requested && !g_wake_requested) daemon_tick(&d, 0);
    }
    bw_log(&d.log, BWLOG_INFO, L"收到停止信号，守护进程结束");
    char pid_utf8[BITWEB_MAX_PATH * 4];
    bw_wide_to_utf8(pidpath, pid_utf8, sizeof(pid_utf8));
    unlink(pid_utf8);
    if (lockfd >= 0) close(lockfd);
    bw_logger_close(&d.log);
    return 0;
}
