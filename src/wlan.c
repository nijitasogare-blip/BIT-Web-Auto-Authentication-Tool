/* wlan.c - 通过 WLAN API 读取当前 SSID（替代 netsh 子进程） */
#include "bitweb.h"
#include <wlanapi.h>
#include <stdlib.h>
#include <string.h>

static void set_err(wchar_t *err, int cap, const wchar_t *msg, DWORD code) {
    if (!err || cap <= 0) return;
    if (code) {
        _snwprintf(err, cap - 1, L"%ls（错误码 %lu）", msg, (unsigned long)code);
    } else {
        wcsncpy(err, msg, cap - 1);
    }
    err[cap - 1] = 0;
}

static void ssid_bytes_to_wide(const unsigned char *bytes, int len, wchar_t *out, int cap) {
    if (len <= 0) { out[0] = 0; return; }
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)bytes, len, out, cap - 1);
    if (n <= 0) {
        n = MultiByteToWideChar(CP_ACP, 0, (const char *)bytes, len, out, cap - 1);
    }
    if (n <= 0) {
        out[0] = 0;
        return;
    }
    out[n] = 0;
}

int bw_wlan_query(BWWlan *items, int max_items, int *count,
                  wchar_t *err, int errcap) {
    *count = 0;
    if (err && errcap > 0) err[0] = 0;
    DWORD version = 0;
    HANDLE handle = NULL;
    DWORD rc = WlanOpenHandle(2, NULL, &version, &handle);
    if (rc != ERROR_SUCCESS) {
        set_err(err, errcap, L"无法访问 WLAN 服务（无线网卡/WLAN 服务不可用？）", rc);
        return 0;
    }
    PWLAN_INTERFACE_INFO_LIST list = NULL;
    rc = WlanEnumInterfaces(handle, NULL, &list);
    if (rc != ERROR_SUCCESS) {
        set_err(err, errcap, L"枚举无线网卡失败", rc);
        WlanCloseHandle(handle, NULL);
        return 0;
    }
    for (DWORD i = 0; i < list->dwNumberOfItems && *count < max_items; i++) {
        const WLAN_INTERFACE_INFO *info = &list->InterfaceInfo[i];
        BWWlan *item = &items[*count];
        memset(item, 0, sizeof(*item));
        wcsncpy(item->name, info->strInterfaceDescription, 127);
        item->connected = 0;
        wcscpy(item->state, L"未连接");
        if (info->isState == wlan_interface_state_connected) {
            PVOID data = NULL;
            DWORD data_size = 0;
            WLAN_OPCODE_VALUE_TYPE opcode;
            rc = WlanQueryInterface(handle, &info->InterfaceGuid,
                                    wlan_intf_opcode_current_connection, NULL,
                                    &data_size, &data, &opcode);
            if (rc == ERROR_SUCCESS && data) {
                WLAN_CONNECTION_ATTRIBUTES *attr = (WLAN_CONNECTION_ATTRIBUTES *)data;
                const WLAN_ASSOCIATION_ATTRIBUTES *assoc = &attr->wlanAssociationAttributes;
                ssid_bytes_to_wide(assoc->dot11Ssid.ucSSID,
                                   (int)assoc->dot11Ssid.uSSIDLength,
                                   item->ssid, 128);
                if (item->ssid[0]) {
                    item->connected = 1;
                    wcscpy(item->state, L"已连接");
                }
                WlanFreeMemory(data);
            } else {
                wcscpy(item->state, L"已连接（读取 SSID 失败）");
            }
        }
        (*count)++;
    }
    WlanFreeMemory(list);
    WlanCloseHandle(handle, NULL);
    return 1;
}
