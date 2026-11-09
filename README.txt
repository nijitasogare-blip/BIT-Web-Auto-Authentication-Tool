======================================================================
 BIT-Web Auto Login（BIT-Web 校园网断线自动重连工具）
 Windows 10/11 · Python 3.8+ 标准库 · 无需管理员权限
======================================================================

一、这是什么
----------------------------------------------------------------------
BIT-Web 掉线后需要反复访问 10.0.0.55 重新认证。本工具后台常驻：

  * 只在 WLAN 的 SSID 精确等于 "BIT-Web" 时工作；
  * 连上后立即检查外网，断网则自动向深澜（Srun）门户提交学号/密码重新登录；
  * 保持连接期间每 5 分钟检查一次外网（HTTP + ping baidu.com）；
  * 连到其他无线网、有线网或未联网时休眠，不访问门户、不发送任何个人信息；
  * 开机自动启动（计划任务），带系统托盘 GUI 和终端 TUI；
  * 密码用 Windows DPAPI 加密，日志不记录密码。

详细 Markdown 版见 README.md（如有差异，以 README.md 为准）。

二、安装（3 步）
----------------------------------------------------------------------
1) 在项目目录打开 PowerShell，执行：

   powershell -ExecutionPolicy Bypass -File .\install.ps1

   脚本会：
     - 复制程序到 %LOCALAPPDATA%\BITWebAutoLogin
     - 创建开始菜单快捷方式（控制面板 / 终端面板）
     - 注册两个“登录时启动”的计划任务：
         BITWebAutoLogin      守护进程（pythonw，隐藏窗口）
         BITWebAutoLoginTray  系统托盘控制面板（powershell，隐藏窗口）

2) 设置密码（安装脚本会提示；也可稍后手动执行）：

   python "%LOCALAPPDATA%\BITWebAutoLogin\set_password.py"

   提示输入学号/工号和密码，密码输入不回显，DPAPI 加密后写入 config.json。
   注意：学号/密码必须是 ASCII 字符。

3) 自检：

   python "%LOCALAPPDATA%\BITWebAutoLogin\bitweb_monitor.py" --self-test

如果提示找不到 Python，请安装 Python 3.8+（python.org，安装时勾选
Add python.exe to PATH），或用 -PythonPath "C:\path\to\python.exe" 指定。

可选参数：
   install.ps1 -NoStart -NoPasswordSetup      只安装、不启动、不引导设置密码
   install.ps1 -NoTray                        不自动启动托盘面板
   install.ps1 -InstallDir "D:\Tools\BITWebAutoLogin"

三、界面
----------------------------------------------------------------------
系统托盘 GUI：
  开始菜单 -> BIT-Web 自动登录 -> 控制面板
  或双击系统托盘图标。关闭窗口默认最小化到托盘；右键托盘图标有菜单。
  可查看状态/日志、立即自检、设置密码、重启守护进程、退出。

终端 TUI：
  开始菜单 -> BIT-Web 自动登录 -> 终端面板
  或：python "%LOCALAPPDATA%\BITWebAutoLogin\tui.py"
  按键：R 刷新 S 自检 P 设置密码 L 日志 G 托盘界面 D 重启守护进程 Q 退出

命令行状态：
  python "%LOCALAPPDATA%\BITWebAutoLogin\status_report.py"
  powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\BITWebAutoLogin\status.ps1"

日志：
  %LOCALAPPDATA%\BITWebAutoLogin\logs\bitweb_monitor.log

四、运行逻辑
----------------------------------------------------------------------
  每 15 秒读取一次 netsh wlan show interfaces：
    当前 SSID == BIT-Web ?
      否 -> 休眠：只轮询 SSID，不访问 10.0.0.55、不发送任何信息
              （连续 2 次确认不到才休眠，避免瞬断误判）
      是 -> 唤醒：立即检查外网
              通   -> 记录“连通性正常”
              不通 -> 获取 challenge、计算签名、提交登录、复核外网
                      成功 -> 继续每 5 分钟检查
                      门户提示“IP 已在线”但外网不通 -> 注销残留会话后重登一次
                      密码/账号错误 -> 冷却 30 分钟
                      未配置密码     -> 冷却 10 分钟（提示运行 set_password.py）

五、配置（config.json）
----------------------------------------------------------------------
文件位置：%LOCALAPPDATA%\BITWebAutoLogin\config.json
（仓库中只提供 config.example.json 模板；config.json 已被 .gitignore 排除）

  username                      学号/工号（ASCII）
  password_dpapi                DPAPI 密文，由 set_password.py 写入
  network.allowed_ssids         白名单 SSID，默认 ["BIT-Web"]，大小写敏感、精确匹配
  network.ssid_poll_sec         轮询 SSID 间隔，默认 15 秒
  network.sleep_confirm_polls   连续几次检测不到才休眠，默认 2
  connectivity.check_interval_sec  外网检查间隔，默认 300 秒
  connectivity.ping_targets     ping 目标，默认 ["baidu.com", "114.114.114.114"]
  connectivity.http_probe       是否 HTTP 探测，默认 true
  login.retry_count             每次断网重试次数，默认 2
  login.auth_error_backoff_sec  密码/账号错误冷却，默认 1800 秒
  logging.file                  日志路径，默认 logs/bitweb_monitor.log

修改用户名/密码后热加载生效，无需重启守护进程；修改其他配置建议重启守护进程。

六、安全与隐私
----------------------------------------------------------------------
  * 只在 SSID 精确等于 BIT-Web 时工作；相似/仿冒 SSID 不会触发认证；
  * 其他无线网、有线网、未连接时休眠，不向门户发送任何请求；
  * 密码 DPAPI 加密，绑定当前 Windows 用户与本机，配置被拷走也无法还原；
  * 日志只记录脱敏学号（如 2024******）与状态，不记录密码；
  * 认证请求只发往 10.0.0.55；外网 HTTP 探测仅用于判断连通性；
  * 已知残余风险：门户是 HTTP 明文，无法防御“伪造同名 BIT-Web 热点 +
    假门户”的攻击。请勿连接来源不明的同名热点。

七、常见问题
----------------------------------------------------------------------
1. 日志显示“尚未设置密码”
   运行 python "%LOCALAPPDATA%\BITWebAutoLogin\set_password.py"

2. 提示“疑似账号/密码问题，30 分钟内不再自动重试”
   核对密码后重新运行 set_password.py，冷却自动解除；也可手动登录一次。

3. 计划任务没运行
   运行 status.ps1 查看；任务计划程序库根目录有 BITWebAutoLogin 和
   BITWebAutoLoginTray。安全软件拦截时请加白名单或以管理员重装。

4. ping 不通但网页能上
   校园网常屏蔽 ICMP，工具默认先 HTTP 探测、失败再 ping，不会误判。

5. 托盘图标不见了
   从开始菜单重新打开控制面板，或检查 BITWebAutoLoginTray 任务状态。

6. 换 Windows 用户 / 换电脑
   DPAPI 密文无法跨用户解密，重新运行 set_password.py。

7. 学号或密码含中文
   当前协议实现只支持 ASCII 凭据，请改用 ASCII 密码。

8. 学校门户升级
   协议实现位于 srun_client.py，需要按新门户前端 JS 更新；测试向量可用
   node tests/js_reference.js 重新生成。

9. 完全卸载
   powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\BITWebAutoLogin\uninstall.ps1"
   powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\BITWebAutoLogin\uninstall.ps1" -RemoveData
   （第二条会二次确认后删除程序、配置和日志）

八、开发与测试
----------------------------------------------------------------------
  cd bitweb-autologin
  python -m unittest discover -s tests -v
  node tests\js_reference.js      # 重新生成 JS 参考向量

也可用 --once 调试：
  python .\bitweb_monitor.py --once --verbose
  python .\bitweb_monitor.py --once --simulate-ssid "SomeOtherWiFi"
  python .\bitweb_monitor.py --once --simulate-offline --dry-run

九、已知限制
----------------------------------------------------------------------
  * 15 秒轮询粒度感知网络变化（非系统网络事件驱动）；
  * 按需求只处理 BIT-Web 无线，有线网络不自动认证；
  * 门户若新增验证码/双因子，自动登录会失败并进入冷却；
  * 仅支持 Windows（netsh、DPAPI、计划任务、WinForms）。

十、许可证
----------------------------------------------------------------------
MIT License，详见 LICENSE。本项目为个人效率工具，与北京理工大学官方无关；
请仅用于自己的账号。
