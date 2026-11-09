# BIT-Web Auto Login（原生 C 版）

> 北京理工大学 `BIT-Web` 校园网断线自动重连工具 · **Windows 10/11 + Linux**
> **生产实现：纯 C（Windows 用 Win32，Linux 用 POSIX）；无 Python、无重型运行时依赖**
> Python 实现保留在 [`python-reference/`](python-reference/)，仅用于原理验证与测试向量。

`BIT-Web` 频繁掉线后需要反复打开 `10.0.0.55` 重新认证，会打断远程桌面 / SSH / 科研任务。
本工具在后台常驻：**只在连上 BIT-Web 时工作**，每 5 分钟确认一次外网；一旦断网就自动向
深澜（Srun）认证门户提交学号/密码重新登录。其他无线网、有线网、未联网时自动休眠，
不访问门户、不发送任何个人信息。

## 为什么是原生 C 版？

Python 版 GUI 采用 PowerShell + 每次刷新 `python status_report.py` 子进程的方式，
刷新时 UI 会卡顿（子进程启动 + 脚本解释 + schtasks 查询约数百毫秒到数秒）。
原生 C 版把全部能力放进进程内：

| 能力 | Python 版 | 原生 C 版 |
|---|---|---|
| SSID 读取 | `netsh` 子进程 | WLAN API（进程内，毫秒级） |
| 外网探测 | Python 子进程 + urllib | Winsock HTTP / ICMP API（进程内） |
| 门户认证 | Python 子进程 | WinHTTP + 自实现签名（进程内） |
| GUI 刷新 | 拉起 Python 脚本 | 后台线程采集，UI 线程不阻塞 |
| 开机自启 | Python + 计划任务 | 单个 `bitweb.exe` + 计划任务 |
| 运行时依赖 | Python 3.8+ | 无（仅 Windows 系统 DLL） |
| 体积 | 多文件脚本 | `bitweb.exe` 约 160 KB + `bitweb-cli.exe` 约 140 KB |

## 功能特性

- **原生 C 守护进程**：登录 Windows 后由计划任务自动启动，无窗口、失败自动重启；
- **系统托盘 GUI（Windows）**：双击托盘图标随时呼出控制面板；关闭窗口最小化到托盘；
- **Linux CLI + TUI + systemd**：终端控制面板、用户级 systemd 服务、systemd 自启；
- **终端 TUI**：`bitweb-cli.exe tui`，在 Windows Terminal / cmd 中按键操作；
- **精确 SSID 判断**：只有 WLAN SSID 精确等于 `BIT-Web` 才工作（区分大小写），其他网络一律休眠；
- **每 5 分钟外网检查**：HTTP 探测（识别被劫持到认证门户）+ `ping baidu.com` 兜底；
- **自动重新认证**：断网后完成深澜 Srun 签名登录，并复核外网是否恢复；
- **残留会话处理**：门户提示“该 IP 已在线”但外网不通时，自动注销残留会话后重登；
- **失败保护**：密码错误冷却 30 分钟，未配置密码冷却 10 分钟；
- **密码加密**：Windows DPAPI（当前用户作用域）；**与 Python 版配置/密文完全兼容**；
- **配置热加载**：修改密码后无需重启守护进程；
- **手动刷新立即重连**：GUI/TUI 刷新时若检测到断网，通过命名事件通知守护进程立即认证；
- **非白名单网络只读检测**：连到其他 SSID / 有线网时休眠（不访问门户、不认证），但仍每 5 分钟做一次**只读**连通性检测并展示结果。

## 构建

只依赖 **MSYS2 UCRT64 的 gcc**（或任意 x86_64-w64-mingw32 gcc）：

```powershell
# 安装 MSYS2 后：pacman -S mingw-w64-ucrt-x86_64-gcc
powershell -ExecutionPolicy Bypass -File .\build.ps1
# 产物：bin\bitweb.exe、bin\bitweb-cli.exe
```

> 不想自行编译？从 GitHub Releases 下载预编译的 `bitweb.exe` / `bitweb-cli.exe`。
>
> 提示：如果从本仓库目录直接运行 exe 时 WLAN 读取被拒绝（ACCESS_DENIED），
> 请先 `install.ps1` 安装到 `%LOCALAPPDATA%\BITWebAutoLogin` 后再运行（确保与 TUI/GUI 的运行目录一致）。

## Linux 版

Linux 版为纯 C/POSIX 实现，与 Windows 共用协议与配置字段，提供 **CLI + TUI + 后台守护 + systemd 用户服务**（暂无桌面托盘 GUI）。

```bash
cd linux
make
./install.sh                  # 安装到 ~/.local/bin + systemd 用户服务（无需 root）
~/.local/bin/bitweb set-password
~/.local/bin/bitweb status
~/.local/bin/bitweb tui
```

- SSID 检测依赖 NetworkManager 的 `nmcli`（可用 `iwgetid` 回退）；无 NetworkManager 的环境会休眠，只做只读外网检测；
- 密码保存在 0600 权限的 `~/.config/bitweb-autologin/config.json`（Linux 无 DPAPI；不要与 Windows 的 `password_dpapi` 混用）；
- 手动 `bitweb reconnect` 或 TUI 刷新检测到断网时，立即通知守护进程重连；
- 非白名单网络每 5 分钟只做只读连通性检测，不访问门户、不认证；
- 测试：`bash linux/tests/run_tests.sh`（本地假门户 + 模拟 SSID，不依赖真实校园网）。

详见 [linux/README.md](linux/README.md)。
## 安装

```powershell
# 1) 安装：复制程序到 %LOCALAPPDATA%\BITWebAutoLogin，注册两个开机自启任务，创建开始菜单快捷方式
powershell -ExecutionPolicy Bypass -File .\install.ps1

# 2) 设置密码（安装脚本会检测；未设置时手动执行）
& "$env:LOCALAPPDATA\BITWebAutoLogin\bitweb-cli.exe" set-password

# 3) 自检
& "$env:LOCALAPPDATA\BITWebAutoLogin\bitweb-cli.exe" self-test
```

安装脚本创建两个“登录时启动”的计划任务：

- `BITWebAutoLogin`：`bitweb.exe daemon`（无窗口守护进程）；
- `BITWebAutoLoginTray`：`bitweb.exe --silent`（系统托盘控制面板）。

不需要管理员权限；计划任务被策略拦截时会退回启动文件夹快捷方式。
**不会覆盖已有 `config.json`**：Python 版配置可以直接沿用（DPAPI 密文通用）。

可选参数：

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1 -NoStart -NoTray
powershell -ExecutionPolicy Bypass -File .\install.ps1 -InstallDir "D:\Tools\BITWebAutoLogin"
```

## 界面与命令

| 功能 | 方式 |
|---|---|
| 系统托盘 GUI | 开始菜单 → *BIT-Web 自动登录* → **控制面板**；或双击托盘图标 |
| 终端 TUI | 开始菜单 → **终端面板**；或 `bitweb-cli.exe tui` |
| 命令行状态 | `bitweb-cli.exe status` |
| 安装自检 | `bitweb-cli.exe self-test` |
| 设置/更新密码 | `bitweb-cli.exe set-password` |
| 协议签名离线测试 | `bitweb-cli.exe crypto-test` |
| 登录请求测试 | `bitweb-cli.exe login-test`（已在线时返回 “IP 已在线”） |
| 立即检查/重连 | `bitweb-cli.exe reconnect`；或点击面板“刷新状态” |

托盘 GUI 面板包含：用户名、密码状态、白名单、当前 SSID、监听状态、外网连通性、
守护进程/托盘状态、开机自启、最近日志，以及刷新/自检/设置密码/重启守护进程/打开日志等按钮。
定时刷新只做本地采集（无网络请求），点“刷新状态”才联网检测，保证面板不卡；若刷新发现断网，会立即通知守护进程尝试重连（无需等到下一个 5 分钟周期）。

## 运行逻辑

```
每 15 秒读取 WLAN SSID（WLAN API）
│
├─ 当前 SSID == BIT-Web ?
│   ├─ 否  → 休眠：只轮询 SSID + 每 5 分钟只读连通性检测；不访问 10.0.0.55、不认证
│   │        （连续 2 次确认不到才休眠，避免瞬断误判）
│   └─ 是  → 唤醒：立即检查外网连通性
│            ├─ 通 → 记录“连通性正常”
│            └─ 不通 → 获取 challenge → 计算签名 → 提交登录 → 复核外网
│                      ├─ 成功 → 继续每 5 分钟检查
│                      ├─ 门户提示“IP 已在线”但外网不通 → 注销残留会话后重登一次
│                      ├─ 密码/账号错误 → 冷却 30 分钟
│                      └─ 未配置密码 → 冷却 10 分钟
│
└─ 保持 BIT-Web 期间每 300 秒重复外网检查
```

认证协议（与 `10.0.0.55` 深澜门户实际下发的前端 JS 一致，已用独立 JS 向量逐位验证）：

```
GET  /cgi-bin/get_challenge?username=...&ip=...          → challenge(token)
info   = "{SRBX1}" + base64_custom(xEncode(JSON{username,password,ip,acid,enc_ver}, token))
hmd5   = HMAC-MD5(key=token, msg=password)
chksum = SHA1(token+user+token+hmd5+token+ac_id+token+ip+token+200+token+1+token+info)
GET  /cgi-bin/srun_portal?action=login&password={MD5}... → {"error":"ok"} 即成功
GET  /cgi-bin/srun_portal?action=logout&ac_id=...&ip=... → 注销残留会话
```

## 配置参考（`config.json`）

文件位置：`%LOCALAPPDATA%\BITWebAutoLogin\config.json`（安装时由 `config.example.json` 生成）。
仓库中的 `config.json` 已被 `.gitignore` 排除，只提供无个人信息的模板。

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `username` | `""` | 学号 / 工号（必须是 ASCII） |
| `password_dpapi` | `""` | `set-password` 写入的 DPAPI 密文，勿手改 |
| `portal.host` | `"10.0.0.55"` | 认证门户地址 |
| `portal.ac_id` | `""` | 留空自动从门户页面识别（当前为 43） |
| `network.allowed_ssids` | `["BIT-Web"]` | 白名单 SSID，大小写敏感、精确匹配 |
| `network.ssid_poll_sec` | `15` | 轮询 SSID 间隔（秒） |
| `network.sleep_confirm_polls` | `2` | 连续几次检测不到白名单才休眠 |
| `connectivity.check_interval_sec` | `300` | 外网连通性检查间隔（秒） |
| `connectivity.ping_targets` | `["baidu.com","114.114.114.114"]` | ping 目标，任一成功即在线 |
| `connectivity.http_probe` | `true` | HTTP 探测（识别被劫持到认证门户） |
| `connectivity.http_probe_url` | `"http://www.baidu.com/"` | HTTP 探测地址（Winsock 实现，只用 IPv4） |
| `login.retry_count` | `2` | 每次断网最多重试次数 |
| `login.auth_error_backoff_sec` | `1800` | 密码/账号错误后的冷却时间（秒） |
| `logging.file` | `"logs\\bitweb_monitor.log"` | 日志路径（相对配置文件目录） |

## 安全与隐私

- **只在 BIT-Web 上工作**：SSID 精确匹配（区分大小写），相似/仿冒 SSID 不会触发认证；
  其他网络一律休眠，不向门户发送任何请求；
- **密码不落明文**：DPAPI 加密绑定当前 Windows 用户与本机；换用户/换机器需重新设置；
- **日志脱敏**：只记录脱敏学号（如 `2024******`）与状态，不记录密码；
- **只发内网**：认证请求只发往 `10.0.0.55`；外网探测只用于判断连通性；
- **已知残余风险**：门户为 HTTP 明文，且按需求以 SSID 判断网络，无法防御“伪造同名
  BIT-Web 热点 + 假门户”的攻击；请勿连接来源不明的同名热点。

## 常见问题

**1. 手动点击“刷新状态”会立刻重连吗？**
会。刷新检测到“白名单网络 + 外网不通”时会发信号给守护进程立即认证；非白名单网络只做只读检测，不会认证。

**2. 日志显示“尚未设置密码” / 冷却 10 分钟**
运行 `bitweb-cli.exe set-password`；修改后立即生效，无需重启守护进程。

**3. 提示“疑似账号/密码问题，30 分钟内不再自动重试”**
核对密码后重新运行 `set-password`，冷却会自动解除。

**4. 计划任务没有运行**
运行 `status.ps1` 查看任务状态；任务计划程序库根目录有 `BITWebAutoLogin` 与
`BITWebAutoLoginTray`。安全软件拦截时请把 `bitweb.exe` 加白名单。

**4. 从仓库目录直接运行 exe 报 WLAN ACCESS_DENIED**
这是部分受限环境对非安装位置程序的限制；执行 `install.ps1` 安装到
`%LOCALAPPDATA%\BITWebAutoLogin` 后即可正常运行。

**5. 换 Windows 用户 / 换电脑**
DPAPI 密文无法跨用户解密，重新运行 `set-password`。

**6. 学号或密码含中文**
当前协议实现只支持 ASCII 凭据（与门户 JS 的编码方式严格对齐），请改用 ASCII 密码。

**7. 学校门户升级 / 换主题**
协议实现位于 `src/srun.c`、`src/crypto.c`；需要用新的门户前端 JS 更新并重新生成
`python-reference/tests/js_reference_expected.json` 测试向量。

**8. 如何完全卸载**
```powershell
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\BITWebAutoLogin\uninstall.ps1"
# 连配置和日志一起删除（会二次确认）
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\BITWebAutoLogin\uninstall.ps1" -RemoveData
```
> `-RemoveData` 出于安全考虑只允许删除默认安装目录；自定义 `-InstallDir` 请手动删除。

## 项目结构

```
bitweb-autologin/
├── src/                      # 原生 C 源码
│   ├── bitweb.h              # 公共声明
│   ├── main_gui.c            # bitweb.exe 入口（GUI/托盘/daemon）
│   ├── gui.c                 # Win32 托盘 + 控制面板
│   ├── daemon.c              # SSID 状态机 + 自动认证守护进程
│   ├── main_cli.c            # bitweb-cli.exe 入口（status/tui/self-test/...）
│   ├── tui.c                 # 终端控制面板
│   ├── srun.c                # 深澜门户协议
│   ├── crypto.c              # MD5/SHA1/HMAC-MD5/xEncode/签名
│   ├── http.c                # WinHTTP 客户端（门户请求）
│   ├── net.c                 # Winsock HTTP 探测 + ICMP ping
│   ├── wlan.c                # WLAN API 读取 SSID
│   ├── config.c              # 配置/DPAPI
│   ├── json.c                # 轻量 JSON 解析
│   ├── util.c                # 文件/日志/路径等
│   └── status.c              # 状态采集/自检文本
├── linux/                    # Linux 原生 C 实现（CLI/TUI + systemd）
├── python-reference/         # 旧 Python 实现，仅作原理验证与测试向量
├── tests/                    # 见 python-reference/tests
├── build.ps1                 # gcc 构建脚本
├── install.ps1 / uninstall.ps1 / status.ps1
├── config.example.json       # 无个人信息的配置模板
├── README.md / README.txt
└── .github/workflows/        # CI：C 构建 + 签名向量 + Python 参考测试
```

## 开发与测试

```powershell
# 构建
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean

# 协议签名向量（与门户 JS 参考实现逐位一致）
.\bin\bitweb-cli.exe crypto-test

# Python 参考实现测试（39 项）
python -m unittest discover -s python-reference\tests -v

# 重新生成 JS 参考向量
node python-reference\tests\js_reference.js
```

CI（GitHub Actions）在 Windows 上使用 MSYS2 构建 C 版本并运行 `crypto-test`，在 Ubuntu 上构建 Linux 版并运行假门户集成测试，
同时在 Windows/Linux 上运行 Python 参考测试、校验 JS 向量与 PowerShell 语法。

## 已知限制

- 按需求只处理 **BIT-Web 无线**：有线网络下不自动认证；
- 面板定时刷新不联网（保证不卡），联网检测需要手动点“刷新状态”或由守护进程完成；
- 门户若新增验证码 / 双因子，自动登录会失败并进入冷却，需要人工处理；
- 仅支持 Windows（WLAN API、DPAPI、计划任务、WinForms/Direct Win32）。

## License

[MIT](LICENSE)。本项目为个人效率工具，与北京理工大学官方无关；请仅用于自己的账号。