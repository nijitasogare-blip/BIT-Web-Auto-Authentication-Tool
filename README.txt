======================================================================
 BIT-Web Auto Login（原生 C 版）
 Windows 10/11 + Linux · 纯 C · 无 Python、无运行时依赖
 Windows：GUI/托盘 + daemon；Linux：CLI/TUI + systemd
======================================================================

一、这是什么
----------------------------------------------------------------------
BIT-Web 掉线后需要反复访问 10.0.0.55 重新认证。本工具后台常驻：

  * 只在 WLAN 的 SSID 精确等于 "BIT-Web" 时工作；
  * 连上后立即检查外网，断网则自动向深澜（Srun）门户提交学号/密码重新登录；
  * 保持连接期间每 5 分钟检查一次外网（HTTP + ping baidu.com）；
  * 连到其他无线网、有线网或未联网时休眠，不访问门户、不发送任何个人信息；
  * 开机自动启动（计划任务），带系统托盘 GUI 和终端 TUI；
  * 手动点击“刷新状态”检测到断网时，立即通知守护进程重连；
  * 非 BIT-Web 网络只做只读连通性检测（不访问门户、不认证）；
  * 密码用 Windows DPAPI 加密，日志不记录密码。

为什么用原生 C：Python 版 GUI 每次刷新都要启动 Python 子进程，界面会卡顿；
C 版把 SSID/HTTP/认证/GUI 全部放进进程内，面板刷新不再阻塞。

详细 Markdown 版见 README.md（如有差异，以 README.md 为准）。

二、构建
----------------------------------------------------------------------
需要 MSYS2 UCRT64 的 gcc（或任意 x86_64-w64-mingw32 gcc）：

  pacman -S mingw-w64-ucrt-x86_64-gcc     # 在 MSYS2 中安装
  powershell -ExecutionPolicy Bypass -File .\build.ps1

产物：bin\bitweb.exe（GUI/托盘 + daemon）、bin\bitweb-cli.exe（命令行/TUI）。
不想编译可从 GitHub Releases 下载预编译 exe。

三、安装（2 步）
----------------------------------------------------------------------
1) 安装并注册开机自启（不需要管理员权限）：

   powershell -ExecutionPolicy Bypass -File .\install.ps1

   安装到 %LOCALAPPDATA%\BITWebAutoLogin，注册两个计划任务：
     BITWebAutoLogin      bitweb.exe daemon（无窗口守护进程）
     BITWebAutoLoginTray  bitweb.exe --silent（系统托盘控制面板）
   并在开始菜单创建“控制面板 / 终端面板 / 自检”快捷方式。
   不会覆盖已有 config.json（Python 版配置可直接沿用）。

2) 设置密码（安装脚本会提示；也可手动执行）：

   & "%LOCALAPPDATA%\BITWebAutoLogin\bitweb-cli.exe" set-password

   密码用 DPAPI 加密保存，输入不回显；学号/密码必须是 ASCII 字符。

可选参数：
   install.ps1 -NoStart -NoTray
   install.ps1 -InstallDir "D:\Tools\BITWebAutoLogin"

四、界面与命令
----------------------------------------------------------------------
系统托盘 GUI：
  开始菜单 -> BIT-Web 自动登录 -> 控制面板（或双击托盘图标）
  关闭窗口最小化到托盘；右键托盘图标有菜单。
  面板：状态、外网、守护进程、日志，刷新/自检/设置密码/重启守护进程。

终端 TUI：
  开始菜单 -> 终端面板；或 bitweb-cli.exe tui
  按键：R 刷新 S 自检 P 设置密码 L 日志 G 托盘界面 D 重启 Q 退出

命令行：
  bitweb-cli.exe status        显示状态（会联网检测外网）
  bitweb-cli.exe self-test     安装自检
  bitweb-cli.exe set-password  设置/更新密码
  bitweb-cli.exe crypto-test   协议签名离线测试（与门户 JS 向量对拍）
  bitweb-cli.exe login-test    登录请求测试（已在线时返回“IP 已在线”）
  bitweb-cli.exe reconnect     通知守护进程立即检查/重连

日志：
  %LOCALAPPDATA%\BITWebAutoLogin\logs\bitweb_monitor.log

五、运行逻辑
----------------------------------------------------------------------
  每 15 秒读取 WLAN SSID：
    当前 SSID == BIT-Web ?
      否 -> 休眠：只轮询 SSID + 每 5 分钟只读连通性检测，不访问门户、不认证
              （连续 2 次确认不到才休眠，避免瞬断误判）
      是 -> 唤醒：立即检查外网
              通   -> 记录“连通性正常”
              不通 -> 获取 challenge、计算签名、提交登录、复核外网
                      成功 -> 继续每 5 分钟检查
                      门户提示“IP 已在线”但外网不通 -> 注销残留会话后重登一次
                      密码/账号错误 -> 冷却 30 分钟
                      未配置密码     -> 冷却 10 分钟

六、配置（config.json）
----------------------------------------------------------------------
文件位置：%LOCALAPPDATA%\BITWebAutoLogin\config.json
（仓库只提供 config.example.json 模板；config.json 已被 .gitignore 排除）

  username                      学号/工号（ASCII）
  password_dpapi                DPAPI 密文，由 set-password 写入
  network.allowed_ssids         白名单 SSID，默认 ["BIT-Web"]，大小写敏感
  network.ssid_poll_sec         轮询 SSID 间隔，默认 15 秒
  network.sleep_confirm_polls   连续几次检测不到才休眠，默认 2
  connectivity.check_interval_sec  外网检查间隔，默认 300 秒
  connectivity.ping_targets     ping 目标，默认 ["baidu.com","114.114.114.114"]
  connectivity.http_probe       是否 HTTP 探测，默认 true
  login.retry_count             每次断网重试次数，默认 2
  login.auth_error_backoff_sec  密码/账号错误冷却，默认 1800 秒
  logging.file                  日志路径，默认 logs\bitweb_monitor.log

修改用户名/密码后热加载生效，无需重启守护进程。

七、安全与隐私
----------------------------------------------------------------------
  * 只在 SSID 精确等于 BIT-Web 时工作；相似/仿冒 SSID 不会触发认证；
  * 其他无线网、有线网、未连接时休眠，不向门户发送任何请求；
  * 密码 DPAPI 加密，绑定当前 Windows 用户与本机；
  * 日志只记录脱敏学号（如 2024******）与状态，不记录密码；
  * 认证请求只发往 10.0.0.55；外网 HTTP 探测仅用于判断连通性；
  * 已知残余风险：门户是 HTTP 明文，无法防御“伪造同名 BIT-Web 热点 +
    假门户”的攻击。请勿连接来源不明的同名热点。

八、常见问题
----------------------------------------------------------------------
1. 日志显示“尚未设置密码”
   运行 bitweb-cli.exe set-password

2. 提示“疑似账号/密码问题，30 分钟内不再自动重试”
   核对密码后重新运行 set-password，冷却自动解除。

3. 计划任务没运行
   运行 status.ps1 查看；任务计划程序中可找到两个 BITWebAutoLogin* 任务。

4. 从仓库目录直接运行 exe 报 WLAN ACCESS_DENIED
   部分受限环境限制非安装位置程序；执行 install.ps1 安装到
   %LOCALAPPDATA%\BITWebAutoLogin 后即可正常运行。

5. 换 Windows 用户 / 换电脑
   DPAPI 密文无法跨用户解密，重新运行 set-password。

6. 学号或密码含中文
   当前协议只支持 ASCII 凭据，请改用 ASCII 密码。

7. 学校门户升级
   协议实现位于 src/srun.c、src/crypto.c，需要用新门户前端 JS 更新，
   并重新生成 python-reference/tests/js_reference_expected.json。

8. 完全卸载
   powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\BITWebAutoLogin\uninstall.ps1"
   powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\BITWebAutoLogin\uninstall.ps1" -RemoveData
   （第二条会二次确认后删除程序、配置和日志）

九、开发与测试
----------------------------------------------------------------------
  powershell -ExecutionPolicy Bypass -File .\build.ps1 -Clean
  .\bin\bitweb-cli.exe crypto-test
  python -m unittest discover -s python-reference\tests -v
  node python-reference\tests\js_reference.js

Python 实现保留在 python-reference\，仅用于原理验证和生成测试向量，
生产版本不依赖 Python。

十、已知限制
----------------------------------------------------------------------
  * 按需求只处理 BIT-Web 无线，有线网络不自动认证；
  * 面板定时刷新不联网（保证不卡），联网检测需点“刷新状态”；
  * 门户若新增验证码/双因子，自动登录会失败并进入冷却；
  * 仅支持 Windows。

十一、许可证
----------------------------------------------------------------------
MIT License，详见 LICENSE。本项目为个人效率工具，与北京理工大学官方无关；
请仅用于自己的账号。