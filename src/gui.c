/* gui.c - 原生 Win32 控制面板（系统托盘 + 状态窗口，无子进程/无脚本）
 *
 * 性能要点：
 *  - 状态在后台线程采集，UI 线程永不阻塞；
 *  - SSID 用 WLAN API、HTTP 用 WinHTTP、ping 用 ICMP API，全部进程内调用；
 *  - 定时刷新只读本地状态（无网络探测），点“刷新状态”才联网。
 */
#include "bitweb.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_APP_REFRESH_DONE  (WM_APP + 1)
#define WM_APP_TRAY          (WM_APP + 2)
#define WM_APP_SELFTEST_DONE (WM_APP + 3)
#define WM_APP_RESTART_DONE  (WM_APP + 4)

#define ID_REFRESH   101
#define ID_SELFTEST  102
#define ID_PASSWORD  103
#define ID_RESTART   104
#define ID_LOG       105
#define ID_FOLDER    106
#define ID_HIDE      107
#define ID_EXIT      108
#define ID_PW_USER   201
#define ID_PW_PASS   202
#define ID_PW_PASS2  203
#define ID_PW_OK     204
#define ID_PW_CANCEL 205

#define IDM_SHOW     301
#define IDM_REFRESH  302
#define IDM_SELFTEST 303
#define IDM_PASSWORD 304
#define IDM_RESTART  305
#define IDM_LOG      306
#define IDM_EXIT     307

static HINSTANCE g_hinst;
static HWND g_hwnd;
static NOTIFYICONDATAW g_nid;
static HFONT g_font, g_font_mono;
static HBRUSH g_bg_brush;
static HANDLE g_gui_mutex;
static int g_silent;
static int g_exiting;
static int g_balloon_shown;
static int g_dpi = 96;

static volatile LONG g_worker_running;
static volatile LONG g_refresh_with_network;
static CRITICAL_SECTION g_lock;
static BWStatus g_status;
static wchar_t g_selftest[65536];

static HWND g_values[9];
static HWND g_log_edit;
static HWND g_updated;

static const wchar_t *WND_CLASS = L"BITWebAutoLoginPanel";
static const wchar_t *PW_CLASS = L"BITWebAutoLoginPwDlg";

#define S(x) ((int)(((double)(x)) * g_dpi / 96.0))

static HICON app_icon(void) {
    HICON icon = (HICON)LoadImageW(NULL, IDI_SHIELD, IMAGE_ICON, 0, 0,
                                   LR_SHARED | LR_DEFAULTSIZE);
    if (!icon) icon = LoadIconW(NULL, IDI_APPLICATION);
    return icon;
}

static int task_installed(void) {
    wchar_t path[MAX_PATH];
    UINT n = GetWindowsDirectoryW(path, MAX_PATH);
    if (n == 0) return 0;
    swprintf(path + n, MAX_PATH - n, L"\\System32\\Tasks\\BITWebAutoLogin");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void set_label(int index, const wchar_t *text) {
    if (index >= 0 && index < 9 && g_values[index]) {
        SetWindowTextW(g_values[index], text);
        InvalidateRect(g_values[index], NULL, TRUE);
    }
}

static void set_label2(int index, const wchar_t *prefix, const wchar_t *value) {
    wchar_t line[1024];
    _snwprintf(line, 1023, L"%ls%ls", prefix, value ? value : L"");
    line[1023] = 0;
    set_label(index, line);
}

/* ------------------------- 后台状态采集 ------------------------- */
static DWORD WINAPI status_thread(LPVOID param) {
    (void)param;
    BWStatus st;
    memset(&st, 0, sizeof(st));
    BWConfig cfg;
    wchar_t err[256] = { 0 };
    if (bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
        bw_status_collect(&cfg, &st, g_refresh_with_network ? 1 : 0);
    } else {
        st.config_ok = 0;
        wcsncpy(st.config_path, g_bitweb_config_path,
                (sizeof(st.config_path) / sizeof(wchar_t)) - 1);
        wcsncpy(st.net_detail, err, 511);
    }
    EnterCriticalSection(&g_lock);
    g_status = st;
    LeaveCriticalSection(&g_lock);
    PostMessage(g_hwnd, WM_APP_REFRESH_DONE, 0, 0);
    InterlockedExchange(&g_worker_running, 0);
    return 0;
}

static void start_refresh(int with_network) {
    g_refresh_with_network = with_network ? 1 : 0;
    if (InterlockedCompareExchange(&g_worker_running, 1, 0) == 0) {
        HANDLE t = CreateThread(NULL, 0, status_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        else InterlockedExchange(&g_worker_running, 0);
    }
}

static void apply_status(void) {
    BWStatus st;
    EnterCriticalSection(&g_lock);
    st = g_status;
    LeaveCriticalSection(&g_lock);
    if (!st.config_ok) {
        set_label2(0, L"配置文件读取失败：", st.net_detail);
        return;
    }
    set_label2(0, L"用户名：", st.username_masked);
    set_label2(1, L"密码：", st.password_configured ? L"已配置（DPAPI 加密）" : L"未配置（点“设置密码”）");
    set_label2(2, L"白名单：", st.allowed);
    set_label2(3, L"当前 SSID：", st.connected_ssids[0] ? st.connected_ssids : L"未连接");
    set_label2(4, L"监听状态：", st.in_whitelist ? L"工作中" : L"休眠（不访问门户）");
    if (st.connectivity_checked) {
        wchar_t line[1024];
        _snwprintf(line, 1023, L"外网：%ls（%ls）",
                   st.online ? L"正常" : L"不通", st.net_detail);
        line[1023] = 0;
        set_label(5, line);
    } else {
        set_label2(5, L"外网：", st.in_whitelist ? L"未检测（点“刷新状态”）" : L"未检测（休眠中）");
    }
    set_label2(6, L"守护进程：", st.daemon_running ? L"运行中" : L"未运行");
    set_label2(7, L"托盘界面：", L"运行中");
    set_label2(8, L"开机自启：", task_installed() ? L"已安装计划任务" : L"未检测到计划任务");

    if (g_log_edit) {
        SetWindowTextW(g_log_edit, st.log_tail);
        SendMessageW(g_log_edit, EM_SETSEL, 0, (LPARAM)-1);
        SendMessageW(g_log_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    }
    wchar_t line[256];
    _snwprintf(line, 255, L"最后刷新：%ls", st.generated);
    if (g_updated) SetWindowTextW(g_updated, line);
}

/* ------------------------- 自检 / 重启 ------------------------- */
static DWORD WINAPI selftest_thread(LPVOID param) {
    (void)param;
    wchar_t *buf = (wchar_t *)calloc(65536, sizeof(wchar_t));
    if (buf) {
        bw_selftest_build(buf, 65536);
        EnterCriticalSection(&g_lock);
        wcsncpy(g_selftest, buf, 65535);
        g_selftest[65535] = 0;
        LeaveCriticalSection(&g_lock);
        free(buf);
    }
    PostMessage(g_hwnd, WM_APP_SELFTEST_DONE, 0, 0);
    return 0;
}

static void run_selftest(void) {
    HANDLE t = CreateThread(NULL, 0, selftest_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static DWORD WINAPI restart_thread(LPVOID param) {
    (void)param;
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, BITWEB_STOP_EVENT);
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    for (int i = 0; i < 30; i++) {
        if (!bw_running_mutex_exists(BITWEB_DAEMON_MUTEX)) break;
        Sleep(100);
    }
    wchar_t dir[BITWEB_MAX_PATH * 2], exe[BITWEB_MAX_PATH * 2], cmd[BITWEB_MAX_PATH * 4];
    int ok = 0;
    if (bw_get_exe_dir(dir, (int)(sizeof(dir) / sizeof(wchar_t)))) {
        swprintf(exe, (int)(sizeof(exe) / sizeof(wchar_t)), L"%ls\\bitweb.exe", dir);
        if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES) {
            swprintf(cmd, (int)(sizeof(cmd) / sizeof(wchar_t)), L"\"%ls\" daemon", exe);
            STARTUPINFOW si;
            PROCESS_INFORMATION pi;
            memset(&si, 0, sizeof(si));
            si.cb = sizeof(si);
            memset(&pi, 0, sizeof(pi));
            if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
                ok = 1;
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
    PostMessage(g_hwnd, WM_APP_RESTART_DONE, ok, 0);
    return 0;
}

static void restart_daemon(void) {
    HANDLE t = CreateThread(NULL, 0, restart_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ------------------------- 设置密码对话框 ------------------------- */
static int g_pw_ok;

static LRESULT CALLBACK pw_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HWND user, pass, pass2;
    switch (msg) {
        case WM_CREATE: {
            int y = S(16);
            CreateWindowExW(0, L"STATIC", L"学号 / 工号：", WS_CHILD | WS_VISIBLE,
                            S(16), y, S(110), S(22), hwnd, NULL, g_hinst, NULL);
            user = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   S(130), y - S(2), S(220), S(26), hwnd, (HMENU)ID_PW_USER, g_hinst, NULL);
            y += S(36);
            CreateWindowExW(0, L"STATIC", L"密码：", WS_CHILD | WS_VISIBLE,
                            S(16), y, S(110), S(22), hwnd, NULL, g_hinst, NULL);
            pass = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD,
                                   S(130), y - S(2), S(220), S(26), hwnd, (HMENU)ID_PW_PASS, g_hinst, NULL);
            y += S(36);
            CreateWindowExW(0, L"STATIC", L"再输一次：", WS_CHILD | WS_VISIBLE,
                            S(16), y, S(110), S(22), hwnd, NULL, g_hinst, NULL);
            pass2 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD,
                                    S(130), y - S(2), S(220), S(26), hwnd, (HMENU)ID_PW_PASS2, g_hinst, NULL);
            y += S(46);
            CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            S(130), y, S(100), S(30), hwnd, (HMENU)ID_PW_OK, g_hinst, NULL);
            CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            S(250), y, S(100), S(30), hwnd, (HMENU)ID_PW_CANCEL, g_hinst, NULL);
            {
                BWConfig cfg;
                wchar_t err[256];
                if (bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
                    SetWindowTextW(user, cfg.username);
                }
            }
            SendMessageW(user, WM_SETFONT, (WPARAM)g_font, TRUE);
            SendMessageW(pass, WM_SETFONT, (WPARAM)g_font, TRUE);
            SendMessageW(pass2, WM_SETFONT, (WPARAM)g_font, TRUE);
            SetFocus(user);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == ID_PW_OK) {
                wchar_t username[128], p1[256], p2[256];
                GetWindowTextW(user, username, 128);
                GetWindowTextW(pass, p1, 256);
                GetWindowTextW(pass2, p2, 256);
                if (!username[0]) {
                    MessageBoxW(hwnd, L"学号不能为空。", L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                if (!p1[0]) {
                    MessageBoxW(hwnd, L"密码不能为空。", L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                if (wcscmp(p1, p2) != 0) {
                    MessageBoxW(hwnd, L"两次输入的密码不一致。", L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                char pw_utf8[1024];
                bw_wide_to_utf8(p1, pw_utf8, sizeof(pw_utf8));
                if (!bw_ascii_only(pw_utf8)) {
                    MessageBoxW(hwnd, L"密码包含非 ASCII 字符，当前协议实现不支持。",
                                L"提示", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                char b64[4096];
                if (!bw_dpapi_protect((const unsigned char *)pw_utf8, (int)strlen(pw_utf8),
                                      b64, sizeof(b64))) {
                    MessageBoxW(hwnd, L"DPAPI 加密失败。", L"错误", MB_OK | MB_ICONERROR);
                    return 0;
                }
                BWConfig cfg;
                wchar_t err[256];
                if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
                    bw_config_defaults(&cfg);
                }
                wcsncpy(cfg.username, username, 127);
                strncpy(cfg.password_dpapi, b64, sizeof(cfg.password_dpapi) - 1);
                cfg.password_dpapi[sizeof(cfg.password_dpapi) - 1] = 0;
                cfg.password_plain[0] = 0;
                if (!bw_config_save(&cfg, g_bitweb_config_path)) {
                    MessageBoxW(hwnd, L"保存配置失败。", L"错误", MB_OK | MB_ICONERROR);
                    return 0;
                }
                g_pw_ok = 1;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == ID_PW_CANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static void show_password_dialog(void) {
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = pw_proc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;
    wc.lpszClassName = PW_CLASS;
    RegisterClassExW(&wc);

    RECT parent;
    GetWindowRect(g_hwnd, &parent);
    int w = S(390), h = S(220);
    int x = parent.left + ((parent.right - parent.left) - w) / 2;
    int y = parent.top + ((parent.bottom - parent.top) - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, PW_CLASS, L"设置 BIT-Web 自动登录密码",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               x, y, w, h, g_hwnd, NULL, g_hinst, NULL);
    if (!dlg) return;
    g_pw_ok = 0;
    EnableWindow(g_hwnd, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(g_hwnd, TRUE);
    SetActiveWindow(g_hwnd);
    if (g_pw_ok) {
        MessageBoxW(g_hwnd, L"密码已加密保存。守护进程下次登录时会自动读取。",
                    L"完成", MB_OK | MB_ICONINFORMATION);
        start_refresh(0);
    }
}

/* ------------------------- 托盘 ------------------------- */
static void show_balloon(const wchar_t *title, const wchar_t *text) {
    g_nid.uFlags |= NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title, 63);
    wcsncpy(g_nid.szInfo, text, 255);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
}

static void show_window(void) {
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    start_refresh(0);
}

static void hide_to_tray(int balloon) {
    ShowWindow(g_hwnd, SW_HIDE);
    if (balloon && !g_balloon_shown) {
        g_balloon_shown = 1;
        show_balloon(L"BIT-Web 自动登录",
                     L"控制面板已最小化到系统托盘，双击图标可随时打开。");
    }
}

static void show_tray_menu(void) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"打开控制面板");
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"刷新状态（联网检测）");
    AppendMenuW(menu, MF_STRING, IDM_SELFTEST, L"立即自检");
    AppendMenuW(menu, MF_STRING, IDM_PASSWORD, L"设置密码");
    AppendMenuW(menu, MF_STRING, IDM_RESTART, L"重启守护进程");
    AppendMenuW(menu, MF_STRING, IDM_LOG, L"打开日志");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);
}

static void open_log(void) {
    BWConfig cfg;
    wchar_t err[256];
    if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) return;
    wchar_t path[BITWEB_MAX_PATH * 2];
    bw_resolve_relative(g_bitweb_config_path, cfg.log_file, path,
                        (int)(sizeof(path) / sizeof(wchar_t)));
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwnd, L"日志文件还不存在。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(g_hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

static void open_folder(void) {
    wchar_t dir[BITWEB_MAX_PATH * 2];
    bw_get_exe_dir(dir, (int)(sizeof(dir) / sizeof(wchar_t)));
    ShellExecuteW(g_hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

/* ------------------------- 窗口过程 ------------------------- */
static void create_controls(HWND hwnd) {
    const wchar_t *names[] = {
        L"用户名：--", L"密码：--", L"白名单：--", L"当前 SSID：--",
        L"监听状态：--", L"外网：--", L"守护进程：--", L"托盘界面：--",
        L"开机自启：--"
    };
    for (int i = 0; i < 9; i++) {
        g_values[i] = CreateWindowExW(0, L"STATIC", names[i], WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      S(16), S(12 + i * 23), S(670), S(21),
                                      hwnd, NULL, g_hinst, NULL);
        SendMessageW(g_values[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    HWND caption = CreateWindowExW(0, L"STATIC", L"最近日志", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   S(16), S(224), S(200), S(20), hwnd, NULL, g_hinst, NULL);
    SendMessageW(caption, WM_SETFONT, (WPARAM)g_font, TRUE);
    g_log_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                 ES_READONLY | ES_AUTOVSCROLL,
                                 S(16), S(246), S(670), S(250), hwnd, NULL, g_hinst, NULL);
    SendMessageW(g_log_edit, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
    g_updated = CreateWindowExW(0, L"STATIC", L"最后刷新：--", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                S(16), S(504), S(400), S(20), hwnd, NULL, g_hinst, NULL);
    SendMessageW(g_updated, WM_SETFONT, (WPARAM)g_font, TRUE);

    struct { const wchar_t *text; int id; int x, y, w; } buttons[] = {
        { L"刷新状态", ID_REFRESH, 16, 532, 96 },
        { L"立即自检", ID_SELFTEST, 120, 532, 96 },
        { L"设置密码", ID_PASSWORD, 224, 532, 96 },
        { L"重启守护进程", ID_RESTART, 328, 532, 120 },
        { L"打开日志", ID_LOG, 456, 532, 100 },
        { L"安装目录", ID_FOLDER, 564, 532, 100 },
        { L"最小化到托盘", ID_HIDE, 16, 570, 120 },
        { L"退出", ID_EXIT, 144, 570, 80 },
    };
    for (int i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++) {
        HWND b = CreateWindowExW(0, L"BUTTON", buttons[i].text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 S(buttons[i].x), S(buttons[i].y), S(buttons[i].w), S(30),
                                 hwnd, (HMENU)(INT_PTR)buttons[i].id, g_hinst, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
}

static void add_tray_icon(HWND hwnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = app_icon();
    wcsncpy(g_nid.szTip, L"BIT-Web 自动登录", 127);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            create_controls(hwnd);
            add_tray_icon(hwnd);
            SetTimer(hwnd, 1, 4000, NULL);
            start_refresh(1);
            return 0;
        case WM_TIMER:
            if (wp == 1) start_refresh(0);
            return 0;
        case WM_APP_REFRESH_DONE:
            apply_status();
            return 0;
        case WM_APP_SELFTEST_DONE: {
            wchar_t *text = (wchar_t *)calloc(65536, sizeof(wchar_t));
            if (text) {
                EnterCriticalSection(&g_lock);
                wcscpy(text, g_selftest);
                LeaveCriticalSection(&g_lock);
                MessageBoxW(hwnd, text, L"自检结果", MB_OK | MB_ICONINFORMATION);
                free(text);
            }
            return 0;
        }
        case WM_APP_RESTART_DONE:
            MessageBoxW(hwnd, wp ? L"守护进程已重新启动。" : L"重启守护进程失败，请重新运行 install.ps1。",
                        L"提示", MB_OK | (wp ? MB_ICONINFORMATION : MB_ICONWARNING));
            start_refresh(0);
            return 0;
        case WM_APP_TRAY:
            if (lp == WM_LBUTTONDBLCLK) show_window();
            else if (lp == WM_RBUTTONUP) show_tray_menu();
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            switch (id) {
                case ID_REFRESH: start_refresh(1); break;
                case ID_SELFTEST: run_selftest(); break;
                case ID_PASSWORD: show_password_dialog(); break;
                case ID_RESTART: restart_daemon(); break;
                case ID_LOG: open_log(); break;
                case ID_FOLDER: open_folder(); break;
                case ID_HIDE: hide_to_tray(1); break;
                case ID_EXIT: g_exiting = 1; DestroyWindow(hwnd); break;
                case IDM_SHOW: show_window(); break;
                case IDM_REFRESH: start_refresh(1); break;
                case IDM_SELFTEST: run_selftest(); break;
                case IDM_PASSWORD: show_password_dialog(); break;
                case IDM_RESTART: restart_daemon(); break;
                case IDM_LOG: open_log(); break;
                case IDM_EXIT: g_exiting = 1; DestroyWindow(hwnd); break;
                default: break;
            }
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE) {
                hide_to_tray(1);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (!g_exiting) {
                hide_to_tray(1);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)g_bg_brush;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void init_dpi(void) {
    SetProcessDPIAware();
    HDC dc = GetDC(NULL);
    if (dc) {
        g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    if (g_dpi < 96) g_dpi = 96;
}

static void init_fonts(void) {
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (!g_font) {
        g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    }
    g_font_mono = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FIXED_PITCH | FF_MODERN, L"Consolas");
}

int bw_gui_run(int silent) {
    g_silent = silent;
    g_hinst = GetModuleHandleW(NULL);
    init_dpi();
    InitializeCriticalSection(&g_lock);
    init_fonts();
    g_bg_brush = GetSysColorBrush(COLOR_BTNFACE);

    if (!bw_ensure_single_instance(BITWEB_GUI_MUTEX, &g_gui_mutex)) {
        HWND existing = FindWindowW(WND_CLASS, NULL);
        if (existing) {
            ShowWindow(existing, SW_SHOWNORMAL);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    HICON icon = app_icon();
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_hinst;
    wc.hIcon = icon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;
    wc.lpszClassName = WND_CLASS;
    wc.hIconSm = icon;
    if (!RegisterClassExW(&wc)) return 1;

    int w = S(704), h = S(640);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_hwnd = CreateWindowExW(0, WND_CLASS, L"BIT-Web 自动登录（原生 C 版）",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             x, y, w, h, NULL, NULL, g_hinst, NULL);
    if (!g_hwnd) return 1;

    if (!silent) {
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        UpdateWindow(g_hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_gui_mutex) CloseHandle(g_gui_mutex);
    DeleteCriticalSection(&g_lock);
    if (g_font) DeleteObject(g_font);
    if (g_font_mono) DeleteObject(g_font_mono);
    return 0;
}
