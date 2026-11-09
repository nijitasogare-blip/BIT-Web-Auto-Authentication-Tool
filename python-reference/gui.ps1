#Requires -Version 5.1
<#
.SYNOPSIS
    BIT-Web 自动登录 - Windows 图形控制面板（系统托盘常驻）。

.DESCRIPTION
    双击托盘图标可随时呼出面板；关闭窗口默认最小化到托盘而不是退出。
    通过调用 status_report.py 获取状态，调用 set_password.py 设置密码。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\gui.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -WindowStyle Hidden -File .\gui.ps1 -Silent
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'BITWebAutoLogin'),
    [switch]$Silent
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$TaskName = 'BITWebAutoLogin'
$TrayMutexName = 'Local\BITWebAutoLoginTray'

# 单实例：已在托盘运行时直接退出
try {
    $mutex = [System.Threading.Mutex]::new($false, $TrayMutexName)
    if (-not $mutex.WaitOne(0)) {
        if (-not $Silent) {
            [System.Windows.Forms.MessageBox]::Show(
                'BIT-Web 控制面板已经在系统托盘中运行。', 'BIT-Web 自动登录',
                [System.Windows.Forms.MessageBoxButtons]::OK,
                [System.Windows.Forms.MessageBoxIcon]::Information)
        }
        exit 0
    }
} catch {
    # 互斥量创建失败时继续运行
}

$ScriptDir = $PSScriptRoot
$ConfigPath = Join-Path $InstallDir 'config.json'
$ReportScript = Join-Path $InstallDir 'status_report.py'
$MonitorScript = Join-Path $InstallDir 'bitweb_monitor.py'
$PasswordScript = Join-Path $InstallDir 'set_password.py'
$LogPath = Join-Path $InstallDir 'logs\bitweb_monitor.log'

function Resolve-Python {
    $candidates = @()
    $cmd = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }
    $patterns = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python3*\python.exe'),
        (Join-Path $env:ProgramFiles 'Python3*\python.exe'),
        'C:\Python310\python.exe', 'C:\Python311\python.exe', 'C:\Python312\python.exe'
    )
    foreach ($pattern in $patterns) {
        Get-ChildItem $pattern -ErrorAction SilentlyContinue |
            ForEach-Object { $candidates += $_.FullName }
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            $pythonw = Join-Path (Split-Path $candidate -Parent) 'pythonw.exe'
            return [pscustomobject]@{
                Python = $candidate
                PythonW = $(if (Test-Path $pythonw) { $pythonw } else { $candidate })
            }
        }
    }
    return $null
}

$script:Python = Resolve-Python
if ($script:Python) {
    $script:PythonExe = $script:Python.Python
    $script:PythonWExe = $script:Python.PythonW
} else {
    $script:PythonExe = $null
    $script:PythonWExe = $null
}
$script:LastStatus = $null
$script:AllowExit = $false
$script:ReportOutput = $null
$script:BalloonShown = $false

function Show-TextDialog([string]$Title, [string]$Text) {
    $dialog = New-Object System.Windows.Forms.Form
    $dialog.Text = $Title
    $dialog.Size = New-Object System.Drawing.Size(720, 480)
    $dialog.StartPosition = 'CenterParent'
    $dialog.Icon = [System.Drawing.SystemIcons]::Shield
    $box = New-Object System.Windows.Forms.TextBox
    $box.Multiline = $true
    $box.ReadOnly = $true
    $box.ScrollBars = 'Both'
    $box.Dock = 'Fill'
    $box.Font = New-Object System.Drawing.Font('Consolas', 9)
    $box.Text = $Text
    $dialog.Controls.Add($box)
    $close = New-Object System.Windows.Forms.Button
    $close.Text = '关闭'
    $close.Dock = 'Bottom'
    $close.Height = 32
    $close.Add_Click({ param($s, $e) $dialog.Close() })
    $dialog.Controls.Add($close)
    $dialog.ShowDialog($form) | Out-Null
    $dialog.Dispose()
}

function Set-StatusValue([System.Windows.Forms.Label]$Label, [string]$Text, [string]$Color) {
    if ($null -eq $Label) { return }
    $Label.Text = $Text
    if ($Color) {
        $Label.ForeColor = [System.Drawing.ColorTranslator]::FromHtml($Color)
    } else {
        $Label.ForeColor = [System.Drawing.SystemColors]::ControlText
    }
}

function Update-Status {
    param([switch]$ShowError, [switch]$WithNetwork)
    if (-not $script:Python) {
        Set-StatusValue $lblDaemon '未找到 Python，请重新运行 install.ps1' '#C00000'
        return
    }
    if (-not (Test-Path $ReportScript)) {
        Set-StatusValue $lblDaemon "缺少 status_report.py（$ReportScript）" '#C00000'
        return
    }
    try {
        # 用 python.exe（控制台子系统）而不是 pythonw.exe：PowerShell 对 GUI
        # 子系统程序的 stdout 重定向不可靠，会拿不到状态脚本输出。
        # 定时刷新默认不探测外网（--no-network），只有手动刷新/自检才联网，避免每 10 秒做一次网络请求。
        $reportArgs = @($ReportScript, '--config', $ConfigPath, '--json')
        if (-not $WithNetwork) { $reportArgs += '--no-network' }
        $script:ReportOutput = & $script:PythonExe @reportArgs 2>$null
        $raw = ($script:ReportOutput -join "`n")
        if (-not $raw) { throw '状态脚本没有输出' }
        $data = $raw | ConvertFrom-Json
        $script:LastStatus = $data

        Set-StatusValue $lblUser ('用户名：' + $data.username)
        if ($data.password_configured) {
            Set-StatusValue $lblPassword '密码：已配置（DPAPI 加密）' '#107C10'
        } else {
            Set-StatusValue $lblPassword '密码：未配置（点“设置密码”）' '#C79100'
        }
        Set-StatusValue $lblAllowed ('白名单：' + ($data.allowed_ssids -join '、'))

        $wlan = $data.wlan
        if ($wlan.in_whitelist) {
            Set-StatusValue $lblSsid ('当前 SSID：' + $wlan.current_ssid) '#107C10'
            Set-StatusValue $lblMode '监听状态：工作中' '#107C10'
        } else {
            $ssids = ($wlan.connected_ssids -join '、')
            if (-not $ssids) { $ssids = '未连接' }
            Set-StatusValue $lblSsid ('当前 SSID：' + $ssids) '#666666'
            Set-StatusValue $lblMode '监听状态：休眠（不访问门户）' '#666666'
        }

        $conn = $data.connectivity
        if ($conn.checked) {
            if ($conn.online) {
                Set-StatusValue $lblNet ('外网：正常（' + $conn.detail + '）') '#107C10'
            } else {
                Set-StatusValue $lblNet ('外网：不通（' + $conn.detail + '）') '#C00000'
            }
        } else {
            if ($wlan.in_whitelist) {
                Set-StatusValue $lblNet '外网：未检测（点“刷新状态”）' '#666666'
            } else {
                Set-StatusValue $lblNet '外网：未检测（休眠中）' '#666666'
            }
        }

        if ($data.daemon.running) {
            Set-StatusValue $lblDaemon '守护进程：运行中' '#107C10'
        } else {
            Set-StatusValue $lblDaemon '守护进程：未运行' '#C00000'
        }

        if ($data.task.registered) {
            Set-StatusValue $lblTask ('计划任务：' + $data.task.status) '#107C10'
        } else {
            Set-StatusValue $lblTask '计划任务：未注册' '#C79100'
        }
        if ($data.tray.running) {
            Set-StatusValue $lblTray '系统托盘：运行中' '#107C10'
        } else {
            Set-StatusValue $lblTray '系统托盘：未运行' '#666666'
        }

        $logBox.Text = (($data.log_tail) -join "`r`n")
        $lblUpdated.Text = '最后刷新：' + $data.generated_at
    } catch {
        Set-StatusValue $lblDaemon ('状态读取失败：' + $_.Exception.Message) '#C00000'
        if ($ShowError) {
            [System.Windows.Forms.MessageBox]::Show(
                ('读取状态失败：' + "`r`n" + $_.Exception.Message), 'BIT-Web 自动登录',
                [System.Windows.Forms.MessageBoxButtons]::OK,
                [System.Windows.Forms.MessageBoxIcon]::Warning)
        }
    }
}

function Invoke-SelfTest {
    if (-not $script:Python) { return }
    $oldCursor = [System.Windows.Forms.Cursor]::Current
    [System.Windows.Forms.Cursor]::Current = [System.Windows.Forms.Cursors]::WaitCursor
    try {
        $output = & $script:PythonExe $MonitorScript --config $ConfigPath --self-test 2>&1
        $text = ($output | Out-String)
    } catch {
        $text = '自检执行失败：' + $_.Exception.Message
    } finally {
        [System.Windows.Forms.Cursor]::Current = $oldCursor
    }
    Show-TextDialog '自检结果' $text
    Update-Status -WithNetwork
}

function Invoke-SetPassword {
    if (-not $script:Python) { return }
    $arguments = @($PasswordScript, '--config', $ConfigPath)
    try {
        Start-Process -FilePath $script:PythonExe -ArgumentList $arguments `
            -WorkingDirectory $InstallDir -Wait
    } catch {
        [System.Windows.Forms.MessageBox]::Show(
            ('启动设置密码失败：' + $_.Exception.Message), 'BIT-Web 自动登录',
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Warning)
    }
    Update-Status
}

function Restart-Daemon {
    & schtasks.exe /end /tn $TaskName 2>$null | Out-Null
    Start-Sleep -Milliseconds 500
    & schtasks.exe /run /tn $TaskName 2>$null | Out-Null
    Start-Sleep -Milliseconds 800
    Update-Status
}

function Open-Log {
    if (Test-Path $LogPath) {
        Start-Process notepad.exe -ArgumentList $LogPath
    } else {
        [System.Windows.Forms.MessageBox]::Show(
            "日志文件还不存在：`r`n$LogPath", 'BIT-Web 自动登录',
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Information)
    }
}

function Show-Panel {
    $form.Show()
    $form.WindowState = 'Normal'
    $form.Activate()
    Update-Status
}

function Hide-Panel([switch]$Balloon) {
    $form.Hide()
    if ($Balloon -and -not $script:BalloonShown) {
        $script:BalloonShown = $true
        $notify.ShowBalloonTip(2500, 'BIT-Web 自动登录',
            '控制面板已最小化到系统托盘，双击图标可随时打开。',
            [System.Windows.Forms.ToolTipIcon]::Info)
    }
}

# ---------------------------------------------------------------------------
# 主窗口
# ---------------------------------------------------------------------------
$form = New-Object System.Windows.Forms.Form
$form.Text = 'BIT-Web 自动登录'
$form.Size = New-Object System.Drawing.Size(720, 650)
$form.MinimumSize = New-Object System.Drawing.Size(640, 560)
$form.StartPosition = 'CenterScreen'
$form.Icon = [System.Drawing.SystemIcons]::Shield

$statusGroup = New-Object System.Windows.Forms.GroupBox
$statusGroup.Text = '运行状态'
$statusGroup.Dock = 'Top'
$statusGroup.Height = 252

$statusLayout = New-Object System.Windows.Forms.TableLayoutPanel
$statusLayout.Dock = 'Fill'
$statusLayout.ColumnCount = 1
$statusLayout.RowCount = 9
$statusLayout.Padding = New-Object System.Windows.Forms.Padding(8)
for ($i = 0; $i -lt 9; $i++) {
    $statusLayout.RowStyles.Add((New-Object System.Windows.Forms.RowStyle(
        [System.Windows.Forms.SizeType]::Absolute, 24))) | Out-Null
}
$lblUser = New-Object System.Windows.Forms.Label; $lblUser.Text = '用户名：--'
$lblPassword = New-Object System.Windows.Forms.Label; $lblPassword.Text = '密码：--'
$lblAllowed = New-Object System.Windows.Forms.Label; $lblAllowed.Text = '白名单：--'
$lblSsid = New-Object System.Windows.Forms.Label; $lblSsid.Text = '当前 SSID：--'
$lblMode = New-Object System.Windows.Forms.Label; $lblMode.Text = '监听状态：--'
$lblNet = New-Object System.Windows.Forms.Label; $lblNet.Text = '外网：--'
$lblDaemon = New-Object System.Windows.Forms.Label; $lblDaemon.Text = '守护进程：--'
$lblTask = New-Object System.Windows.Forms.Label; $lblTask.Text = '计划任务：--'
$lblTray = New-Object System.Windows.Forms.Label; $lblTray.Text = '系统托盘：--'
foreach ($label in @($lblUser, $lblPassword, $lblAllowed, $lblSsid, $lblMode, $lblNet, $lblDaemon, $lblTask, $lblTray)) {
    $label.AutoSize = $true
    $label.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 9)
    $statusLayout.Controls.Add($label)
}
$statusGroup.Controls.Add($statusLayout)

$logGroup = New-Object System.Windows.Forms.GroupBox
$logGroup.Text = '最近日志'
$logGroup.Dock = 'Fill'
$logGroup.Padding = New-Object System.Windows.Forms.Padding(8)

$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ReadOnly = $true
$logBox.ScrollBars = 'Vertical'
$logBox.Dock = 'Fill'
$logBox.Font = New-Object System.Drawing.Font('Consolas', 9)
$logBox.BackColor = [System.Drawing.Color]::White
$logGroup.Controls.Add($logBox)

$bottomPanel = New-Object System.Windows.Forms.Panel
$bottomPanel.Dock = 'Bottom'
$bottomPanel.Height = 112
$bottomPanel.Padding = New-Object System.Windows.Forms.Padding(8)

$lblUpdated = New-Object System.Windows.Forms.Label
$lblUpdated.AutoSize = $true
$lblUpdated.Text = '最后刷新：--'
$lblUpdated.Location = New-Object System.Drawing.Point(12, 8)
$bottomPanel.Controls.Add($lblUpdated)

function New-ActionButton([string]$Text, [int]$X, [int]$Y, [int]$Width, [scriptblock]$OnClick) {
    $button = New-Object System.Windows.Forms.Button
    $button.Text = $Text
    $button.Location = New-Object System.Drawing.Point($X, $Y)
    $button.Size = New-Object System.Drawing.Size($Width, 30)
    $button.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 9)
    $button.Add_Click($OnClick)
    $bottomPanel.Controls.Add($button)
    return $button
}

$btnRefresh = New-ActionButton '刷新状态' 12 36 92 { Update-Status -WithNetwork -ShowError }
$btnSelfTest = New-ActionButton '立即自检' 110 36 92 { Invoke-SelfTest }
$btnPassword = New-ActionButton '设置密码' 208 36 92 { Invoke-SetPassword }
$btnRestart = New-ActionButton '重启守护进程' 306 36 108 { Restart-Daemon }
$btnLog = New-ActionButton '打开日志' 420 36 92 { Open-Log }
$btnFolder = New-ActionButton '安装目录' 518 36 92 { Start-Process explorer.exe -ArgumentList $InstallDir }
$btnHide = New-ActionButton '最小化到托盘' 12 70 110 { Hide-Panel -Balloon }
$btnExit = New-ActionButton '退出' 128 70 70 {
    $script:AllowExit = $true
    $form.Close()
}

$form.Controls.Add($logGroup)
$form.Controls.Add($statusGroup)
$form.Controls.Add($bottomPanel)

# ---------------------------------------------------------------------------
# 系统托盘
# ---------------------------------------------------------------------------
$notify = New-Object System.Windows.Forms.NotifyIcon
$notify.Icon = [System.Drawing.SystemIcons]::Shield
$notify.Text = 'BIT-Web 自动登录'
$notify.Visible = $true

$menu = New-Object System.Windows.Forms.ContextMenuStrip
$itemOpen = $menu.Items.Add('打开控制面板')
$itemSelfTest = $menu.Items.Add('立即自检')
$itemPassword = $menu.Items.Add('设置密码')
$itemRestart = $menu.Items.Add('重启守护进程')
$itemLog = $menu.Items.Add('打开日志')
$menu.Items.Add('-') | Out-Null
$itemExit = $menu.Items.Add('退出')
$notify.ContextMenuStrip = $menu

$itemOpen.Add_Click({ Show-Panel })
$itemSelfTest.Add_Click({ Invoke-SelfTest })
$itemPassword.Add_Click({ Invoke-SetPassword })
$itemRestart.Add_Click({ Restart-Daemon })
$itemLog.Add_Click({ Open-Log })
$itemExit.Add_Click({
    $script:AllowExit = $true
    $form.Close()
})
$notify.Add_DoubleClick({ Show-Panel })
$notify.Add_MouseClick({
    param($sender, $e)
    if ($e.Button -eq [System.Windows.Forms.MouseButtons]::Left) { Show-Panel }
})

$form.Add_FormClosing({
    param($sender, $e)
    if (-not $script:AllowExit) {
        $e.Cancel = $true
        Hide-Panel -Balloon
    }
})
$form.Add_Resize({
    if ($form.WindowState -eq [System.Windows.Forms.FormWindowState]::Minimized) {
        $form.WindowState = [System.Windows.Forms.FormWindowState]::Normal
        Hide-Panel -Balloon
    }
})
$form.Add_Shown({
    if ($Silent) {
        Hide-Panel
    } else {
        Update-Status -WithNetwork
    }
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 10000
$timer.Add_Tick({ if ($form.Visible) { Update-Status } })
$timer.Start()

try {
    [System.Windows.Forms.Application]::Run($form)
} finally {
    $timer.Stop()
    $notify.Visible = $false
    $notify.Dispose()
    if ($mutex) {
        try { $mutex.ReleaseMutex() } catch { }
        $mutex.Dispose()
    }
}
