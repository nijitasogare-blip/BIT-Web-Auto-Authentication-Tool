/* linux/src/wlan.c - 通过 nmcli / iwgetid 读取当前 SSID（可用 BITWEB_SIMULATE_SSID 模拟） */
#define _GNU_SOURCE
#include "bitweb.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int capture_cmd(const char *cmd, char *out, int cap) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    int n = 0;
    int c;
    while ((c = fgetc(fp)) != EOF && n < cap - 1) out[n++] = (char)c;
    out[n] = 0;
    int rc = pclose(fp);
    return rc == 0 || n > 0;
}

static void unescape_nmcli(const char *in, wchar_t *out, int cap) {
    char buf[512];
    int o = 0;
    for (const char *p = in; *p && o < (int)sizeof(buf) - 1; p++) {
        if (*p == '\\' && p[1]) { buf[o++] = p[1]; p++; }
        else buf[o++] = *p;
    }
    buf[o] = 0;
    bw_utf8_to_wide(buf, out, cap);
}

int bw_wlan_query(BWWlan *items, int max_items, int *count,
                  wchar_t *err, int errcap) {
    *count = 0;
    if (err && errcap > 0) err[0] = 0;

    const char *sim = getenv("BITWEB_SIMULATE_SSID");
    if (sim && *sim) {
        memset(&items[0], 0, sizeof(items[0]));
        wcsncpy(items[0].name, L"Wi-Fi（模拟）", 127);
        bw_utf8_to_wide(sim, items[0].ssid, 128);
        wcscpy(items[0].state, L"已连接");
        items[0].connected = 1;
        *count = 1;
        return 1;
    }

    const char *nmcli = getenv("BITWEB_NMCLI");
    if (!nmcli || !*nmcli) nmcli = "nmcli";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -t -f IN-USE,SSID device wifi 2>/dev/null", nmcli);
    char out[8192];
    int got = capture_cmd(cmd, out, sizeof(out));

    if (got) {
        char *line = out;
        while (line && *line && *count < max_items) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = 0;
                char *active = line;
                while (*active == ' ' || *active == '\t') active++;
                char *ssid = colon + 1;
                if (strcmp(active, "*") == 0 && *ssid && strcmp(ssid, "--") != 0) {
                    memset(&items[*count], 0, sizeof(items[*count]));
                    wcsncpy(items[*count].name, L"Wi-Fi", 127);
                    unescape_nmcli(ssid, items[*count].ssid, 128);
                    wcscpy(items[*count].state, L"已连接");
                    items[*count].connected = 1;
                    (*count)++;
                }
            }
            if (nl) line = nl + 1;
            else break;
        }
    }

    if (*count == 0) {
        char iw[512];
        snprintf(iw, sizeof(iw), "iwgetid -r 2>/dev/null");
        char ssid_out[256];
        if (capture_cmd(iw, ssid_out, sizeof(ssid_out))) {
            char *nl = strchr(ssid_out, '\n');
            if (nl) *nl = 0;
            if (ssid_out[0]) {
                memset(&items[0], 0, sizeof(items[0]));
                wcsncpy(items[0].name, L"Wi-Fi", 127);
                bw_utf8_to_wide(ssid_out, items[0].ssid, 128);
                wcscpy(items[0].state, L"已连接");
                items[0].connected = 1;
                *count = 1;
            }
        }
    }

    if (*count == 0) {
        if (err) {
            wcsncpy(err, L"未找到已连接的 Wi-Fi（nmcli/iwgetid 不可用或未连接）", errcap - 1);
            err[errcap - 1] = 0;
        }
    }
    return 1;
}
