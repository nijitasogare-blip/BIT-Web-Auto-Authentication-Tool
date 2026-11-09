/* linux/src/gui.c - GTK3 控制面板 + AppIndicator 托盘（Linux UI） */
#define _GNU_SOURCE
#include "bitweb.h"
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GtkWidget *g_window;
static GtkWidget *g_labels[9];
static GtkWidget *g_log_view;
static GtkWidget *g_statusbar;
static AppIndicator *g_indicator;
static GtkWidget *g_indicator_menu;
static volatile int g_refreshing = 0;
static volatile int g_quitting = 0;
static int g_silent = 0;
static gint64 g_last_reconnect_us = 0;
static char *g_selftest_text;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static BWStatus g_status;

static gboolean reconnect_timer_cb(gpointer data);
static void show_msg(GtkWidget *parent, GtkMessageType type, const char *text);

static char *w2u(const wchar_t *text) {
    size_t len = wcslen(text);
    char *buf = (char *)malloc(len * 4 + 8);
    if (!buf) return g_strdup("");
    bw_wide_to_utf8(text, buf, (int)(len * 4 + 7));
    return buf;
}

static void show_window(void) {
    gtk_widget_show_all(g_window);
    gtk_window_present(GTK_WINDOW(g_window));
}

static int service_installed(void) {
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[2048];
    if (xdg && *xdg) snprintf(path, sizeof(path), "%s/systemd/user/bitweb.service", xdg);
    else snprintf(path, sizeof(path), "%s/.config/systemd/user/bitweb.service", home ? home : "");
    return access(path, F_OK) == 0;
}

/* ---------------- 后台状态采集 ---------------- */
static gboolean apply_status_cb(gpointer data) {
    (void)data;
    BWStatus st;
    pthread_mutex_lock(&g_lock);
    st = g_status;
    pthread_mutex_unlock(&g_lock);

    if (!st.config_ok) {
        gtk_label_set_text(GTK_LABEL(g_labels[0]), "配置读取失败（请检查 config.json）");
        return G_SOURCE_REMOVE;
    }
    char *u = w2u(st.username_masked);
    char *a = w2u(st.allowed);
    char *ssid = w2u(st.connected_ssids[0] ? st.connected_ssids : L"未连接");
    char *net = w2u(st.net_detail);
    char *sun = w2u(st.generated);
    gchar *line;

    line = g_strdup_printf("用户名：%s", u); gtk_label_set_text(GTK_LABEL(g_labels[0]), line); g_free(line);
    line = g_strdup_printf("密码：%s", st.password_configured ? "已配置" : "未配置（点“设置密码”）");
    gtk_label_set_text(GTK_LABEL(g_labels[1]), line); g_free(line);
    line = g_strdup_printf("白名单：%s", a); gtk_label_set_text(GTK_LABEL(g_labels[2]), line); g_free(line);
    line = g_strdup_printf("当前 SSID：%s", ssid); gtk_label_set_text(GTK_LABEL(g_labels[3]), line); g_free(line);
    line = g_strdup_printf("监听状态：%s", st.in_whitelist ? "工作中" : "休眠（仅检测连通性，不认证）");
    gtk_label_set_text(GTK_LABEL(g_labels[4]), line); g_free(line);
    if (st.connectivity_checked) {
        line = g_strdup_printf("外网：%s（%s%s）", st.online ? "正常" : "不通",
                               net, (st.in_whitelist || !st.connected_ssids[0]) ? "" : "，仅检测");
    } else {
        line = g_strdup_printf("外网：%s", st.in_whitelist ? "未检测（点“刷新状态”）" : "未检测");
    }
    gtk_label_set_text(GTK_LABEL(g_labels[5]), line); g_free(line);
    line = g_strdup_printf("守护进程：%s", st.daemon_running ? "运行中" : "未运行");
    gtk_label_set_text(GTK_LABEL(g_labels[6]), line); g_free(line);
    gtk_label_set_text(GTK_LABEL(g_labels[7]), "桌面界面：运行中（GTK / AppIndicator）");
    gtk_label_set_text(GTK_LABEL(g_labels[8]), service_installed() ? "开机自启：已安装 systemd 服务" : "开机自启：未安装");

    if (g_log_view) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_log_view));
        char *log = w2u(st.log_tail);
        GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW(gtk_widget_get_parent(g_log_view));
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(sw);
        double pos = gtk_adjustment_get_value(adj);
        gtk_text_buffer_set_text(buf, log, -1);
        gtk_adjustment_set_value(adj, pos);
        free(log);
    }
    if (g_statusbar) {
        line = g_strdup_printf("最后刷新：%s", sun);
        guint id = gtk_statusbar_get_context_id(GTK_STATUSBAR(g_statusbar), "status");
        gtk_statusbar_pop(GTK_STATUSBAR(g_statusbar), id);
        gtk_statusbar_push(GTK_STATUSBAR(g_statusbar), id, line);
        g_free(line);
    }
    free(u); free(a); free(ssid); free(net); free(sun);

    if (st.connectivity_checked && !st.online && st.in_whitelist) {
        gint64 now = g_get_monotonic_time();
        if (now - g_last_reconnect_us > 5 * G_USEC_PER_SEC) {
            g_last_reconnect_us = now;
            bw_signal_daemon_wake();
            g_timeout_add(3500, reconnect_timer_cb, NULL);
        }
    }
    return G_SOURCE_REMOVE;
}

static void *refresh_thread(void *arg) {
    int with_network = *(int *)arg;
    free(arg);
    BWConfig cfg;
    wchar_t err[256] = { 0 };
    BWStatus st;
    memset(&st, 0, sizeof(st));
    if (bw_config_load(&cfg, g_bitweb_config_path, err, 256)) {
        bw_status_collect(&cfg, &st, with_network);
    } else {
        st.config_ok = 0;
        wcsncpy(st.config_path, g_bitweb_config_path,
                sizeof(st.config_path) / sizeof(wchar_t) - 1);
    }
    pthread_mutex_lock(&g_lock);
    g_status = st;
    pthread_mutex_unlock(&g_lock);
    g_idle_add(apply_status_cb, NULL);
    g_refreshing = 0;
    return NULL;
}

static void start_refresh(int with_network) {
    if (g_refreshing) return;
    g_refreshing = 1;
    int *arg = (int *)malloc(sizeof(int));
    if (!arg) { g_refreshing = 0; return; }
    *arg = with_network;
    pthread_t th;
    if (pthread_create(&th, NULL, refresh_thread, arg) != 0) {
        free(arg);
        g_refreshing = 0;
        return;
    }
    pthread_detach(th);
}

/* ---------------- 自检 ---------------- */
static gboolean selftest_done_cb(gpointer data) {
    (void)data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("自检结果", GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "关闭", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 520);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buf, g_selftest_text ? g_selftest_text : "", -1);
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), sw, TRUE, TRUE, 6);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return G_SOURCE_REMOVE;
}

static void *selftest_thread(void *arg) {
    (void)arg;
    wchar_t *buf = (wchar_t *)calloc(65536, sizeof(wchar_t));
    if (buf) {
        bw_selftest_build(buf, 65536);
        pthread_mutex_lock(&g_lock);
        free(g_selftest_text);
        g_selftest_text = w2u(buf);
        pthread_mutex_unlock(&g_lock);
        free(buf);
    }
    g_idle_add(selftest_done_cb, NULL);
    return NULL;
}

static void run_selftest(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, selftest_thread, NULL) == 0) pthread_detach(th);
}

/* ---------------- 设置密码 ---------------- */
static void save_password_dialog(GtkWidget *dialog) {
    GtkWidget *grid = g_object_get_data(G_OBJECT(dialog), "grid");
    GtkWidget *user = g_object_get_data(G_OBJECT(dialog), "user");
    GtkWidget *p1 = g_object_get_data(G_OBJECT(dialog), "p1");
    GtkWidget *p2 = g_object_get_data(G_OBJECT(dialog), "p2");
    (void)grid;
    const char *u = gtk_entry_get_text(GTK_ENTRY(user));
    const char *a = gtk_entry_get_text(GTK_ENTRY(p1));
    const char *b = gtk_entry_get_text(GTK_ENTRY(p2));
    if (!*u) { show_msg(dialog, GTK_MESSAGE_WARNING, "用户名不能为空"); return; }
    if (!*a) { show_msg(dialog, GTK_MESSAGE_WARNING, "密码不能为空"); return; }
    if (strcmp(a, b) != 0) { show_msg(dialog, GTK_MESSAGE_WARNING, "两次密码不一致"); return; }
    if (!bw_ascii_only(a)) { show_msg(dialog, GTK_MESSAGE_WARNING, "密码包含非 ASCII 字符"); return; }

    BWConfig cfg;
    wchar_t err[256] = { 0 };
    if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) bw_config_defaults(&cfg);
    bw_utf8_to_wide(u, cfg.username, 128);
    strncpy(cfg.password_plain, a, sizeof(cfg.password_plain) - 1);
    cfg.password_plain[sizeof(cfg.password_plain) - 1] = 0;
    cfg.password_dpapi[0] = 0;
    if (!bw_config_save(&cfg, g_bitweb_config_path)) {
        show_msg(dialog, GTK_MESSAGE_ERROR, "保存配置失败");
        return;
    }
    show_msg(dialog, GTK_MESSAGE_INFO, "已保存（配置文件权限 0600）。守护进程会热加载。");
    gtk_widget_destroy(dialog);
    start_refresh(0);
}

static void open_password_dialog(void) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("设置 BIT-Web 自动登录密码",
        GTK_WINDOW(g_window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "保存", GTK_RESPONSE_OK, "取消", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 12); gtk_widget_set_margin_end(grid, 12);
    gtk_widget_set_margin_top(grid, 12); gtk_widget_set_margin_bottom(grid, 12);

    BWConfig cfg;
    wchar_t err[256] = { 0 };
    char user_utf8[256] = "";
    if (bw_config_load(&cfg, g_bitweb_config_path, err, 256))
        bw_wide_to_utf8(cfg.username, user_utf8, sizeof(user_utf8));

    GtkWidget *u = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(u), user_utf8);
    GtkWidget *p1 = gtk_entry_new(); gtk_entry_set_visibility(GTK_ENTRY(p1), FALSE);
    GtkWidget *p2 = gtk_entry_new(); gtk_entry_set_visibility(GTK_ENTRY(p2), FALSE);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("学号/工号："), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), u, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("密码："), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p1, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("再输一次："), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), p2, 1, 2, 1, 1);
    g_object_set_data(G_OBJECT(dialog), "user", u);
    g_object_set_data(G_OBJECT(dialog), "p1", p1);
    g_object_set_data(G_OBJECT(dialog), "p2", p2);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, TRUE, TRUE, 6);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        save_password_dialog(dialog);
        return;
    }
    gtk_widget_destroy(dialog);
}

/* ---------------- 动作 ---------------- */
static void on_show(GtkWidget *w, gpointer data) { (void)w; (void)data; show_window(); }

static void show_msg(GtkWidget *parent, GtkMessageType type, const char *text) {
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(parent), GTK_DIALOG_MODAL,
                                          type, GTK_BUTTONS_OK, "%s", text);
    gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
}

static void on_refresh(GtkWidget *w, gpointer data) { (void)w; (void)data; start_refresh(1); }

static gboolean refresh_timer_cb(gpointer data) {
    (void)data;
    start_refresh(0);
    return G_SOURCE_CONTINUE;
}

static gboolean reconnect_timer_cb(gpointer data) {
    (void)data;
    start_refresh(1);
    return G_SOURCE_REMOVE;
}

static void on_selftest(GtkWidget *w, gpointer data) { (void)w; (void)data; run_selftest(); }
static void on_password(GtkWidget *w, gpointer data) { (void)w; (void)data; open_password_dialog(); }

static void on_restart(GtkWidget *w, gpointer data) {
    (void)w; (void)data;
    int rc = system("systemctl --user restart bitweb 2>/dev/null");
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(g_window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        rc == 0 ? "已请求重启 bitweb.service。" : "重启失败（可能尚未运行 install.sh 安装服务）。");
    gtk_dialog_run(GTK_DIALOG(m)); gtk_widget_destroy(m);
    start_refresh(0);
}

static void on_open_log(GtkWidget *w, gpointer data) {
    (void)w; (void)data;
    BWConfig cfg; wchar_t err[256];
    if (!bw_config_load(&cfg, g_bitweb_config_path, err, 256)) return;
    wchar_t path[BITWEB_MAX_PATH * 2];
    bw_resolve_relative(g_bitweb_config_path, cfg.log_file, path,
                        sizeof(path) / sizeof(wchar_t));
    char *utf8 = w2u(path);
    char *uri = g_filename_to_uri(utf8, NULL, NULL);
    if (uri) {
        GError *e = NULL;
        gtk_show_uri_on_window(GTK_WINDOW(g_window), uri, GDK_CURRENT_TIME, &e);
        if (e) { g_error_free(e); }
        g_free(uri);
    }
    free(utf8);
}

static void on_quit(GtkWidget *w, gpointer data) {
    (void)w; (void)data;
    g_quitting = 1;
    if (g_indicator) app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_PASSIVE);
    gtk_main_quit();
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *e, gpointer data) {
    (void)w; (void)e; (void)data;
    if (g_quitting) return FALSE;
    gtk_widget_hide(g_window);
    return TRUE;
}

/* ---------------- 托盘 ---------------- */
static void build_menu(void) {
    g_indicator_menu = gtk_menu_new();
    struct { const char *label; GCallback cb; } items[] = {
        { "打开控制面板", G_CALLBACK(on_show) },
        { "刷新状态 / 立即重连", G_CALLBACK(on_refresh) },
        { "立即自检", G_CALLBACK(on_selftest) },
        { "设置密码", G_CALLBACK(on_password) },
        { "重启守护进程", G_CALLBACK(on_restart) },
        { "退出", G_CALLBACK(on_quit) },
    };
    for (int i = 0; i < (int)(sizeof(items) / sizeof(items[0])); i++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(items[i].label);
        g_signal_connect(mi, "activate", items[i].cb, NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_indicator_menu), mi);
    }
    gtk_widget_show_all(g_indicator_menu);
    g_indicator = app_indicator_new("bitweb", "network-wireless",
                                    APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(g_indicator, "BIT-Web 自动登录");
    app_indicator_set_menu(g_indicator, GTK_MENU(g_indicator_menu));
}

int bw_gui_run(int silent) {
    g_silent = silent;
    { int argc = 1; char *arg0 = (char *)"bitweb-gui"; char **argv = &arg0; gtk_init(&argc, &argv); }

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "BIT-Web 自动登录（Linux GTK 版）");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 780, 660);
    gtk_window_set_icon_name(GTK_WINDOW(g_window), "network-wireless");
    g_signal_connect(g_window, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(g_window), vbox);

    GtkWidget *frame = gtk_frame_new("运行状态");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    const char *initial[] = { "用户名：--", "密码：--", "白名单：--", "当前 SSID：--",
                              "监听状态：--", "外网：--", "守护进程：--", "桌面界面：--", "开机自启：--" };
    for (int i = 0; i < 9; i++) {
        g_labels[i] = gtk_label_new(initial[i]);
        gtk_label_set_xalign(GTK_LABEL(g_labels[i]), 0.0);
        gtk_grid_attach(GTK_GRID(grid), g_labels[i], 0, i, 1, 1);
    }
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    GtkWidget *log_frame = gtk_frame_new("最近日志");
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    g_log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(g_log_view), TRUE);
    gtk_container_add(GTK_CONTAINER(sw), g_log_view);
    gtk_container_add(GTK_CONTAINER(log_frame), sw);
    gtk_box_pack_start(GTK_BOX(vbox), log_frame, TRUE, TRUE, 0);

    GtkWidget *btnbox = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(btnbox), GTK_BUTTONBOX_START);
    const char *btns[] = { "刷新状态", "立即自检", "设置密码", "重启守护进程", "打开日志", "退出" };
    GCallback cbs[] = { G_CALLBACK(on_refresh), G_CALLBACK(on_selftest), G_CALLBACK(on_password),
                        G_CALLBACK(on_restart), G_CALLBACK(on_open_log), G_CALLBACK(on_quit) };
    for (int i = 0; i < 6; i++) {
        GtkWidget *b = gtk_button_new_with_label(btns[i]);
        g_signal_connect(b, "clicked", cbs[i], NULL);
        gtk_container_add(GTK_CONTAINER(btnbox), b);
    }
    gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 0);

    g_statusbar = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(vbox), g_statusbar, FALSE, FALSE, 0);
    gtk_widget_show_all(g_window);

    build_menu();
    g_timeout_add_seconds(4, refresh_timer_cb, NULL);
    start_refresh(1);
    if (silent) gtk_widget_hide(g_window);

    gtk_main();
    return 0;
}