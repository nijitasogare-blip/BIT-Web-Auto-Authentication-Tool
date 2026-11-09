/* status.c - 统一状态采集与文本格式化（GUI/TUI/CLI 共用） */
#include "bitweb.h"
#include <string.h>
#include <wchar.h>

void bw_status_collect(const BWConfig *cfg, BWStatus *st, int with_network) {
    memset(st, 0, sizeof(*st));
    st->config_ok = 1;
    wcsncpy(st->config_path, g_bitweb_config_path,
            (sizeof(st->config_path) / sizeof(wchar_t)) - 1);
    st->password_configured = bw_config_configured_password(cfg);
    bw_mask_user(cfg->username, st->username_masked, 160);

    int o = 0;
    for (int i = 0; i < cfg->allowed_ssid_count; i++) {
        o += swprintf(st->allowed + o, (int)(sizeof(st->allowed) / sizeof(wchar_t)) - o,
                      L"%ls%ls", i ? L"、" : L"", cfg->allowed_ssids[i]);
        if (o >= (int)(sizeof(st->allowed) / sizeof(wchar_t)) - 8) break;
    }

    BWWlan items[16];
    int count = 0;
    wchar_t err[256] = { 0 };
    if (bw_wlan_query(items, 16, &count, err, 256)) {
        wchar_t connected[512] = { 0 };
        for (int i = 0; i < count; i++) {
            if (!items[i].connected || !items[i].ssid[0]) continue;
            st->wlan_connected = 1;
            if (connected[0]) wcsncat(connected, L"、",
                                      (sizeof(connected) / sizeof(wchar_t)) - wcslen(connected) - 1);
            wcsncat(connected, items[i].ssid,
                    (sizeof(connected) / sizeof(wchar_t)) - wcslen(connected) - 1);
            for (int k = 0; k < cfg->allowed_ssid_count; k++) {
                if (wcscmp(cfg->allowed_ssids[k], items[i].ssid) == 0) {
                    wcsncpy(st->current_ssid, items[i].ssid, 127);
                    st->in_whitelist = 1;
                    break;
                }
            }
        }
        wcsncpy(st->connected_ssids, connected, 511);
        if (count > 0) {
            _snwprintf(st->wlan_state, 255, L"%ls：%ls", items[0].name, items[0].state);
        }
    } else {
        wcsncpy(st->wlan_state, err, 255);
    }

    /* 只读连通性检测：无论当前是否白名单 SSID，都判断外网是否可达；
       非白名单时绝不访问 10.0.0.55 门户、绝不提交认证。 */
    if (with_network) {
        st->connectivity_checked = 1;
        st->online = bw_connectivity_check(cfg, st->net_detail, 512);
    }

    st->daemon_running = bw_running_mutex_exists(BITWEB_DAEMON_MUTEX);
    st->tray_running = bw_running_mutex_exists(BITWEB_GUI_MUTEX);
    bw_format_time(st->generated, 64);

    wchar_t log_path[BITWEB_MAX_PATH * 2];
    bw_resolve_relative(g_bitweb_config_path, cfg->log_file, log_path,
                        (int)(sizeof(log_path) / sizeof(wchar_t)));
    bw_status_read_log_tail(log_path, 20, st->log_tail,
                            (int)(sizeof(st->log_tail) / sizeof(wchar_t)));
}

void bw_status_format(const BWStatus *st, wchar_t *out, int cap) {
    int o = 0;
#define APPEND(...) do { \
        int n = swprintf(out + o, cap - o, __VA_ARGS__); \
        if (n > 0) o += n; \
        if (o >= cap - 2) { out[cap - 1] = 0; return; } \
    } while (0)

    APPEND(L"BIT-Web 自动登录（Linux 原生 C 版）  v%ls\n", BITWEB_VERSION);
    APPEND(L"生成时间  : %ls\n", st->generated);
    APPEND(L"配置文件  : %ls\n", st->config_path);
    APPEND(L"用户名    : %ls\n", st->username_masked);
    APPEND(L"密码      : %ls\n", st->password_configured ? L"已配置（DPAPI 加密）" : L"未配置");
    APPEND(L"白名单    : %ls\n", st->allowed);
    APPEND(L"当前 SSID : %ls\n", st->connected_ssids[0] ? st->connected_ssids : L"未连接");
    APPEND(L"监听状态  : %ls\n", st->in_whitelist ? L"工作中" : L"休眠（仅检测连通性，不认证）");
    if (st->connectivity_checked) {
        APPEND(L"外网连通  : %ls（%ls%ls）\n", st->online ? L"正常" : L"不通",
               st->net_detail, st->in_whitelist ? L"" : L"，仅检测");
    } else {
        APPEND(L"外网连通  : %ls\n", st->net_detail);
    }
    APPEND(L"守护进程  : %ls\n", st->daemon_running ? L"运行中" : L"未运行");
    APPEND(L"桌面界面  : %ls\n", L"未提供（Linux 版使用 CLI/TUI）");
    if (st->log_tail[0]) {
        APPEND(L"\n最近日志:\n%ls\n", st->log_tail);
    }
#undef APPEND
    out[o < cap ? o : cap - 1] = 0;
}

/* 自检文本（GUI 面板与 CLI 共用） */
void bw_selftest_build(wchar_t *out, int cap) {
    int o = 0;
#define SEL_APPEND(...) do { \
        int n = swprintf(out + o, cap - o, __VA_ARGS__); \
        if (n > 0) o += n; \
        if (o >= cap - 2) { out[cap - 1] = 0; return; } \
    } while (0)
#define SEL_CHECK(name, ok, detail) SEL_APPEND(L"[%ls] %-10ls %ls\n", \
        (ok) ? L" OK " : L"FAIL", name, (detail))

    SEL_APPEND(L"============================================================\n");
    SEL_APPEND(L" BIT-Web 自动登录 - 安装自检（原生 C 版）\n");
    SEL_APPEND(L"============================================================\n");

    BWConfig cfg;
    wchar_t err[256] = { 0 };
    int ok = bw_config_load(&cfg, g_bitweb_config_path, err, 256);
    wchar_t line[1024];
    _snwprintf(line, 1023, L"%ls", g_bitweb_config_path);
    SEL_CHECK(L"配置文件", ok, line);
    if (!ok) return;

    wchar_t mask[160];
    bw_mask_user(cfg.username, mask, 160);
    SEL_CHECK(L"用户名", cfg.username[0] != 0, mask);

    char password[512] = { 0 };
    int has_pw = bw_config_password(&cfg, password, sizeof(password));
    SEL_CHECK(L"密码", has_pw, has_pw ? L"已配置（DPAPI 加密）" : L"未配置，请运行 set-password");

    BWWlan items[16];
    int count = 0;
    wchar_t werr[256] = { 0 };
    int wlan_ok = bw_wlan_query(items, 16, &count, werr, 256);
    SEL_CHECK(L"WLAN", wlan_ok, wlan_ok ? L"无线网卡正常" : werr);

    wchar_t active[128] = { 0 }, connected[512] = { 0 };
    for (int i = 0; i < count; i++) {
        if (!items[i].connected || !items[i].ssid[0]) continue;
        if (connected[0]) wcsncat(connected, L"、", 500 - wcslen(connected));
        wcsncat(connected, items[i].ssid, 500 - wcslen(connected));
        for (int k = 0; k < cfg.allowed_ssid_count; k++) {
            if (wcscmp(cfg.allowed_ssids[k], items[i].ssid) == 0 && !active[0]) {
                wcsncpy(active, items[i].ssid, 127);
            }
        }
    }
    SEL_CHECK(L"当前网络", connected[0] != 0, connected[0] ? connected : L"无");

    wchar_t detail[512];
    int online = bw_connectivity_check(&cfg, detail, 512);
    if (active[0]) {
        _snwprintf(line, 1023, L"白名单 SSID：%ls", active);
        SEL_CHECK(L"监听状态", 1, line);
    } else {
        SEL_CHECK(L"监听状态", 1, L"休眠（当前不是白名单 SSID：仅检测连通性，不访问门户）");
    }
    SEL_CHECK(L"外网连通", online, detail);
    if (active[0]) {
        BWPortal portal;
        if (bw_srun_discover(&cfg, &portal, err, 256)) {
            _snwprintf(line, 1023, L"%ls（ac_id=%ls，ip=%ls）",
                       portal.url, portal.ac_id, portal.ip);
            SEL_CHECK(L"认证门户", 1, line);
            wchar_t token[256], cip[64], msg[256];
            if (bw_srun_challenge(&cfg, cfg.username, portal.ip, token, 256,
                                  cip, 64, msg, 256)) {
                _snwprintf(line, 1023, L"challenge 正常（长度 %d）", (int)wcslen(token));
                SEL_CHECK(L"challenge", 1, line);
            } else {
                SEL_CHECK(L"challenge", 0, msg);
            }
        } else {
            SEL_CHECK(L"认证门户", 0, err);
        }
    }

    SEL_CHECK(L"守护进程", bw_running_mutex_exists(BITWEB_DAEMON_MUTEX),
              bw_running_mutex_exists(BITWEB_DAEMON_MUTEX) ? L"运行中" : L"未运行");
    SEL_CHECK(L"桌面界面", 1, L"未提供（Linux 版使用 CLI/TUI）");
    SEL_APPEND(L"\n");
    if (!has_pw) SEL_APPEND(L"结论：还差一步——运行 set-password 设置密码。\n");
    else SEL_APPEND(L"结论：配置可读取。连上 BIT-Web 后守护进程会自动认证；建议断开网络实测一次。\n");
#undef SEL_CHECK
#undef SEL_APPEND
    out[o < cap ? o : cap - 1] = 0;
}
