# BIT-Web Auto Login

> 北京理工大学 `BIT-Web` 校园网断线自动重连工具（Windows）。
> 只使用 **Python 标准库 + PowerShell 5.1**，无需 `pip install`，无需管理员权限。

`BIT-Web` 频繁掉线后需要反复打开 `10.0.0.55` 重新认证，会打断远程桌面 / SSH / 科研任务。
本工具在后台常驻：**只在连上 BIT-Web 时工作**，每 5 分钟确认一次外网；一旦断网就自动向
深澜（Srun）认证门户提交学号/密码重新登录。连到其他无线网、有线网或未联网时自动休眠，
不访问门户、不发送任何个人信息。

## 功能特性

- **开机自启**：登录 Windows 后由计划任务自动启动守护进程，隐藏窗口、失败自动重启；
- **系统托盘 GUI**：双击托盘图标随时呼出控制面板，查看状态、自检、设置密码、重启守护进程；
- **终端 TUI**：在 Windows Terminal / cmd 中按键操作，适合服务器式使用习惯；
- **精确 SSID 判断**：只有当前 WLAN 的 SSID **精确等于** `BIT-Web` 才工作（区分大小写），
  其他 SSID / 有线网络 / 未连接一律休眠，不产生任何门户流量；
- **每 5 分钟连通性检查**：HTTP 探测 + `ping baidu.com`，任一成功即在线（校园网常屏蔽 ICMP）；
- **自动重新认证**：断网后自动完成深澜 Srun 签名登录，并复核外网是否真的恢复；
- **残留会话处理**：门户提示“该 IP 已在线”但外网不通时，自动注销残留会话后重登；
- **失败保护**：密码/账号错误冷却 30 分钟，未配置密码冷却 10 分钟，避免触发风控或刷日志；
- **密码加密存储**：Windows DPAPI（当前用户作用域）加密，日志不记录密码，学号脱敏显示；
- **配置热加载**：修改密码后无需重启守护进程，冷却自动解除。

## 界面

| 界面 | 启动方式 | 说明 |
|---|---|---|
| 系统托盘 GUI | 开始菜单 → *BIT-Web 自动登录* → **控制面板**；或双击托盘图标 | 状态面板、日志、自检、设置密码、重启守护进程 |
| 终端 TUI | 开始菜单 → **终端面板**；或 `python tui.py` | 按键操作：刷新 / 自检 / 设置密码 / 日志 / 重启 |
| 命令行状态 | `python status_report.py` | 输出状态文本，支持 `--json` |

托盘面板关闭时默认最小化到系统托盘；右键托盘图标可打开菜单，退出请用菜单中的“退出”。

## 安装

要求：Windows 10/11，Python 3.8+（<https://www.python.org/downloads/>，安装时勾选
*Add python.exe to PATH*）。只用标准库，无需 pip。

在项目目录中打开 PowerShell：

```powershell
# 1) 安装：复制程序到 %LOCALAPPDATA%\BITWebAutoLogin，注册开机自启，创建开始菜单快捷方式
powershell -ExecutionPolicy Bypass -File .\install.ps1

# 2) 安装脚本会提示设置密码（输入不回显）；也可稍后手动执行：
python "$env:LOCALAPPDATA\BITWebAutoLogin\set_password.py"

# 3) 自检
python "$env:LOCALAPPDATA\BITWebAutoLogin\bitweb_monitor.py" --self-test
```

安装脚本会创建两个“登录时启动”的计划任务：

- `BITWebAutoLogin`：守护进程（`pythonw.exe`，隐藏窗口）；
- `BITWebAutoLoginTray`：系统托盘控制面板（`powershell.exe`，隐藏窗口）。

> 不需要管理员权限。若计划任务被策略拦截，守护进程会自动退回“启动文件夹快捷方式”。

可选参数：

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1 -NoStart -NoPasswordSetup
powershell -ExecutionPolicy Bypass -File .\install.ps1 -NoTray            # 不自动启动托盘
powershell -ExecutionPolicy Bypass -File .\install.ps1 -InstallDir "D:\Tools\BITWebAutoLogin"
```

### 首次配置

运行 `set_password.py` 时会提示输入**学号/工号**（若 config 中为空）和**密码**。
密码用 DPAPI 加密后写入 `config.json` 的 `password_dpapi` 字段，不会保存明文。

也可以直接编辑 `%LOCALAPPDATA%\BITWebAutoLogin\config.json`：

```json
{
  "username": "你的学号",
  "password_dpapi": "",
  "network": { "allowed_ssids": ["BIT-Web"] }
}
```

然后用 `set_password.py` 写入密码。**注意：学号/密码必须是 ASCII 字符**（本工具不支持含中文的凭据）。

## 运行逻辑

```
每 15 秒读取 netsh wlan show interfaces
│
├─ 当前 SSID == BIT-Web ?
│   ├─ 否  → 休眠：只轮询 SSID，不访问 10.0.0.55、不发送任何信息
│   │        （连续 2 次确认不到才休眠，避免瞬断误判）
│   └─ 是  → 唤醒：立即检查外网连通性
│            ├─ 通 → 记录“连通性正常”
│            └─ 不通 → 获取 challenge → 计算签名 → 提交登录 → 复核外网
│                      ├─ 成功 → 继续每 5 分钟检查
│                      ├─ 门户提示“IP 已在线”但外网不通 → 注销残留会话后重登一次
│                      ├─ 密码/账号错误 → 冷却 30 分钟
│                      └─ 未配置密码 → 冷却 10 分钟（提示运行 set_password.py）
│
└─ 保持 BIT-Web 期间每 300 秒重复外网检查
```

认证协议（与 `10.0.0.55` 深澜门户实际下发的前端 JS 一致）：

```
GET  /cgi-bin/get_challenge?username=...&ip=...          → challenge(token)
info   = "{SRBX1}" + base64_custom(xEncode(JSON{username,password,ip,acid,enc_ver}, token))
hmd5   = HMAC-MD5(key=token, msg=password)               ← 门户 js-md5 的两参数形式
chksum = SHA1(token+user+token+hmd5+token+ac_id+token+ip+token+200+token+1+token+info)
GET  /cgi-bin/srun_portal?action=login&password={MD5}... → {"error":"ok"} 即成功
GET  /cgi-bin/srun_portal?action=logout&ac_id=...&ip=... → 注销残留会话
```

## 常用操作

```powershell
# 状态（计划任务、进程、SSID、外网、日志，并做一次自检）
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\BITWebAutoLogin\status.ps1"

# 命令行状态
python "$env:LOCALAPPDATA\BITWebAutoLogin\status_report.py"

# 终端面板
python "$env:LOCALAPPDATA\BITWebAutoLogin\tui.py"

# 托盘面板（手动启动；已在托盘则不会重复启动）
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\BITWebAutoLogin\gui.ps1"

# 日志
Get-Content "$env:LOCALAPPDATA\BITWebAutoLogin\logs\bitweb_monitor.log" -Tail 50 -Encoding UTF8

# 更新密码（改完立即生效，无需重启守护进程）
python "$env:LOCALAPPDATA\BITWebAutoLogin\set_password.py"
```

### 调试 / 演练

```powershell
# 只跑一轮：报告 SSID、连通性；断网时尝试登录
python .\bitweb_monitor.py --once --verbose

# 演练“连到别的 Wi-Fi”：应输出休眠，且完全不访问门户
python .\bitweb_monitor.py --once --simulate-ssid "SomeOtherWiFi"

# 演练“断网”：只报告将登录，不真正提交认证请求（无需设置密码）
python .\bitweb_monitor.py --once --simulate-offline --dry-run
```

> 守护进程正在运行时，`--once` 会自动改为只读模式（不执行登录），避免重复认证。

## 配置参考（`config.json`）

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `username` | `""` | 学号 / 工号（ASCII） |
| `password_dpapi` | `""` | 由 `set_password.py` 写入的 DPAPI 密文，勿手动修改 |
| `portal.host` | `"10.0.0.55"` | 认证门户地址 |
| `portal.ac_id` | `""` | 留空则自动从门户页面识别（当前为 43） |
| `network.allowed_ssids` | `["BIT-Web"]` | 白名单 SSID，**大小写敏感、精确匹配** |
| `network.ssid_poll_sec` | `15` | 轮询 SSID 间隔（秒） |
| `network.sleep_confirm_polls` | `2` | 连续几次检测不到白名单才休眠 |
| `connectivity.check_interval_sec` | `300` | 外网连通性检查间隔（秒） |
| `connectivity.ping_targets` | `["baidu.com","114.114.114.114"]` | ping 目标，任一成功即在线 |
| `connectivity.http_probe` | `true` | HTTP 探测（识别被劫持到认证门户） |
| `connectivity.http_probe_url` | `"http://www.baidu.com/"` | HTTP 探测地址 |
| `login.retry_count` | `2` | 每次断网最多重试次数 |
| `login.auth_error_backoff_sec` | `1800` | 密码/账号错误后的冷却时间（秒） |
| `logging.file` | `"logs/bitweb_monitor.log"` | 日志路径（相对配置文件目录） |

> ⚠️ `config.json` 含学号和个人配置，已被 `.gitignore` 排除；仓库中只提供
> [`config.example.json`](config.example.json) 模板。

## 安全与隐私

- **只在 BIT-Web 上工作**：SSID 精确匹配（区分大小写），`bit-web`、`BIT-Web-5G`
  等相似/仿冒 SSID 不会触发认证，不会泄露账号密码；有线网络和其他 SSID 一律休眠。
- **密码不落明文**：DPAPI 加密绑定当前 Windows 用户与本机；换用户/换机器需要重新设置。
- **日志脱敏**：日志只记录脱敏学号（如 `2024******`）和状态，不记录密码。
- **只发内网**：认证请求只发往 `10.0.0.55`；外网 HTTP 探测仅用于判断连通性。
- **已知残余风险**：门户为 HTTP 明文，且按需求以 SSID 判断网络。无法防御“伪造同名
  BIT-Web 热点 + 劫持到假门户”的攻击——此时任何网页密码登录都可能被截获。请勿连接
  来源不明的同名热点。

## 常见问题

**1. 日志显示“尚未设置密码”**
运行 `python "$env:LOCALAPPDATA\BITWebAutoLogin\set_password.py"`；改完无需重启守护进程。

**2. 提示“疑似账号/密码问题，30 分钟内不再自动重试”**
核对密码后重新运行 `set_password.py`，冷却会自动解除。也可手动登录一次 10.0.0.55。

**3. 计划任务没有运行**
运行 `status.ps1` 查看任务状态；在“任务计划程序”库根目录可找到 `BITWebAutoLogin`
与 `BITWebAutoLoginTray`。若安全软件拦截，请将 `pythonw.exe` / `powershell.exe`
加入白名单，或以管理员身份重新运行 `install.ps1`。

**4. ping 不通但网页能上（校园网屏蔽 ICMP）**
工具默认先做 HTTP 探测、失败再 ping，不会误判断网。也可调整 `connectivity.ping_targets`。

**5. 托盘图标不见了**
从开始菜单重新打开“控制面板”；或检查 `BITWebAutoLoginTray` 任务状态。

**6. 换了 Windows 用户 / 换了电脑**
DPAPI 密文无法跨用户解密，重新运行 `set_password.py` 即可。

**7. 学号或密码含中文**
当前协议实现只支持 ASCII 凭据（严格对齐门户 JS 的编码方式），请改用 ASCII 密码。

**8. 学校门户升级 / 换主题**
协议实现位于 `srun_client.py`；如门户前端认证参数变化，需要用新的前端 JS 更新该文件，
`tests/js_reference.js` 可用于重新生成测试向量。

**9. 如何完全卸载**
```powershell
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\BITWebAutoLogin\uninstall.ps1"
# 连配置和日志一起删除（默认安装目录；会二次确认）
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\BITWebAutoLogin\uninstall.ps1" -RemoveData
```

> 使用自定义 `-InstallDir` 安装时，`uninstall.ps1 -RemoveData` 出于安全考虑只允许删除
> 默认安装目录；自定义目录请先移除自启动项，再手动删除目录。

## 项目结构

```
bitweb-autologin/
├── bitweb_monitor.py          # 守护进程：SSID 状态机、连通性检查、登录调度
├── srun_client.py             # 深澜 Srun 协议客户端（xEncode / 签名 / 登录 / 注销）
├── appconfig.py               # 配置默认值、读写、路径解析
├── dpapi.py                   # Windows DPAPI 加解密
├── set_password.py            # 交互式保存密码（DPAPI 加密）
├── status_report.py           # 统一状态采集（GUI/TUI/CLI 共用，支持 --json）
├── tui.py                     # 终端控制面板
├── gui.ps1                    # 系统托盘控制面板（WinForms）
├── install.ps1 / uninstall.ps1
├── status.ps1
├── config.example.json        # 配置模板（不含个人信息）
├── tests/                     # 离线单元测试 + 独立 JS 参考向量
└── .github/workflows/         # GitHub Actions：Windows/Linux 测试、PS 语法、JS 向量一致性
```

## 开发与测试

```powershell
$env:PYTHONIOENCODING='utf-8'; [Console]::OutputEncoding=[System.Text.Encoding]::UTF8
python -m unittest discover -s tests -v
node tests\js_reference.js                 # 重新生成 JS 参考向量
```

测试向量由 `tests/js_reference.js` 生成：该脚本直接照抄门户下发的
`xEncode` / `s` / `l` 函数，并用 Node 的 `crypto` 计算 HMAC-MD5 / SHA1，
确保 Python 实现的签名与门户前端逐位一致；本地假门户集成测试覆盖
“断网 → 登录 → 复核 → 恢复”与“残留会话 → 注销 → 重登”链路。

## 已知限制

- 通过轮询 SSID 感知网络变化（15 秒粒度），不是系统网络事件驱动；
- 按需求只处理 **BIT-Web 无线**：有线网络下不自动认证；
- 门户若新增验证码 / 双因子，自动登录会失败并进入冷却，需要人工处理；
- 仅支持 Windows（依赖 `netsh`、DPAPI、计划任务、WinForms）。

## License

[MIT](LICENSE)。本项目为个人效率工具，与北京理工大学官方无关；请仅用于自己的账号。
