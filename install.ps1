#Requires -Version 5.1
<#
.SYNOPSIS
    安装 BIT-Web 断线自动登录：后台守护进程 + 系统托盘控制面板 + 开机自启。

.DESCRIPTION
    - 默认安装到 %LOCALAPPDATA%\BITWebAutoLogin
    - 复制程序、创建开始菜单快捷方式（控制面板 / 终端面板）
    - 注册两个「登录时启动」的计划任务：
        BITWebAutoLogin     守护进程（pythonw，隐藏窗口）
        BITWebAutoLoginTray 系统托盘控制面板（powershell，隐藏窗口）
    - 不会覆盖已有的 config.json
    - 如未设置密码，默认会引导运行 set_password.py

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -NoPasswordSetup -NoTray
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'BITWebAutoLogin'),
    [string]$PythonPath = '',
    [switch]$NoPasswordSetup,
    [switch]$NoStart,
    [switch]$NoTray
)

$ErrorActionPreference = 'Stop'
$TaskName = 'BITWebAutoLogin'
$TrayTaskName = 'BITWebAutoLoginTray'
$SourceDir = $PSScriptRoot
$Identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$PowerShellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Resolve-Python {
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($PythonPath) { $candidates.Add($PythonPath) }
    $cmd = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates.Add($cmd.Source) }
    foreach ($pattern in @(
            (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python3*\python.exe'),
            (Join-Path $env:ProgramFiles 'Python3*\python.exe'),
            'C:\Python39\python.exe', 'C:\Python310\python.exe',
            'C:\Python311\python.exe', 'C:\Python312\python.exe'
        )) {
        Get-ChildItem $pattern -ErrorAction SilentlyContinue |
            ForEach-Object { $candidates.Add($_.FullName) }
    }
    $seen = @{}
    foreach ($candidate in $candidates) {
        if (-not $candidate -or $seen.ContainsKey($candidate)) { continue }
        $seen[$candidate] = $true
        if (-not (Test-Path $candidate)) { continue }
        $pythonw = Join-Path (Split-Path $candidate -Parent) 'pythonw.exe'
        if (-not (Test-Path $pythonw)) { continue }
        try {
            $version = & $candidate -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null
            if ($LASTEXITCODE -eq 0 -and $version) {
                $parts = $version.Trim().Split('.')
                if ([int]$parts[0] -ge 3 -and [int]$parts[1] -ge 8) {
                    return [pscustomobject]@{
                        Python = $candidate
                        PythonW = $pythonw
                        Version = $version.Trim()
                    }
                }
            }
        } catch { }
    }
    return $null
}

function New-Shortcut([string]$Path, [string]$Target, [string]$Arguments, [string]$WorkDir, [string]$Icon, [string]$Description) {
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $shortcut.TargetPath = $Target
    $shortcut.Arguments = $Arguments
    $shortcut.WorkingDirectory = $WorkDir
    $shortcut.IconLocation = $Icon
    $shortcut.Description = $Description
    $shortcut.Save()
}

function Stop-MatchingProcess([string]$Pattern) {
    Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe' OR Name = 'python.exe' OR Name = 'powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -match $Pattern } |
        ForEach-Object {
            try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop } catch { }
        }
}

function Remove-Autostart {
    foreach ($name in @($TaskName, $TrayTaskName)) {
        $task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
        if ($task) {
            Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
            Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
        }
    }
    Stop-MatchingProcess 'bitweb_monitor\.py'
    Stop-MatchingProcess 'gui\.ps1'
    $startup = [Environment]::GetFolderPath('Startup')
    foreach ($name in @($TaskName, $TrayTaskName)) {
        $lnkPath = Join-Path $startup "$name.lnk"
        if (Test-Path $lnkPath) { Remove-Item $lnkPath -Force }
    }
}

function Register-LogonTask([string]$Name, [string]$Executable, [string]$Arguments, [string]$Description) {
    $action = New-ScheduledTaskAction -Execute $Executable -Argument $Arguments -WorkingDirectory $InstallDir
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $Identity
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -MultipleInstances IgnoreNew `
        -RestartCount 3 `
        -RestartInterval (New-TimeSpan -Minutes 1) `
        -ExecutionTimeLimit ([TimeSpan]::Zero)
    $principal = New-ScheduledTaskPrincipal -UserId $Identity -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $Name -Action $action -Trigger $trigger `
        -Settings $settings -Principal $principal -Description $Description | Out-Null
}

Write-Host '======================================================'
Write-Host ' BIT-Web 断线自动登录 - 安装程序'
Write-Host '======================================================'
Write-Host "安装目录: $InstallDir"
Write-Host "当前用户: $Identity"

Write-Step '检查 Python 3.8+'
$python = Resolve-Python
if (-not $python) {
    Write-Host '未找到可用的 Python 3.8+（需要 python.exe 与 pythonw.exe）。' -ForegroundColor Red
    Write-Host '请安装 Python（https://www.python.org/downloads/ ，安装时勾选 Add python.exe to PATH），'
    Write-Host '或使用 -PythonPath "C:\path\to\python.exe" 指定路径后重试。'
    exit 1
}
Write-Host ("Python: {0}（{1}）" -f $python.Python, $python.Version)

Write-Step '复制程序文件'
$required = @(
    'bitweb_monitor.py', 'srun_client.py', 'dpapi.py', 'appconfig.py',
    'set_password.py', 'status_report.py', 'tui.py', 'gui.ps1',
    'status.ps1', 'uninstall.ps1', 'install.ps1',
    'README.md', 'README.txt', 'LICENSE'
)
$missing = @()
foreach ($file in $required) {
    if (-not (Test-Path (Join-Path $SourceDir $file))) { $missing += $file }
}
if (-not (Test-Path (Join-Path $SourceDir 'config.example.json'))) {
    $missing += 'config.example.json'
}
if ($missing.Count -gt 0) {
    Write-Host ('缺少文件: {0}（请在完整目录中运行本脚本）' -f ($missing -join ', ')) -ForegroundColor Red
    exit 1
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
foreach ($file in $required) {
    Copy-Item -Path (Join-Path $SourceDir $file) -Destination $InstallDir -Force
}
$configPath = Join-Path $InstallDir 'config.json'
if (-not (Test-Path $configPath)) {
    Copy-Item -Path (Join-Path $SourceDir 'config.example.json') -Destination $configPath -Force
    Write-Host '已生成配置文件 config.json（用户名/密码待填写）'
} else {
    Write-Host '已存在 config.json，保留原有配置'
}
Copy-Item -Path (Join-Path $SourceDir 'config.example.json') -Destination $InstallDir -Force
New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'logs') | Out-Null

$scriptPath = Join-Path $InstallDir 'bitweb_monitor.py'
$guiPath = Join-Path $InstallDir 'gui.ps1'
$daemonArgs = '"{0}" --config "{1}"' -f $scriptPath, $configPath
$trayArgs = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -InstallDir "{1}" -Silent' -f $guiPath, $InstallDir

Write-Step '创建开始菜单快捷方式'
$programs = [Environment]::GetFolderPath('Programs')
$menuDir = Join-Path $programs 'BIT-Web 自动登录'
New-Item -ItemType Directory -Force -Path $menuDir | Out-Null
$iconShield = Join-Path $env:SystemRoot 'System32\imageres.dll,78'
New-Shortcut -Path (Join-Path $menuDir '控制面板.lnk') `
    -Target $PowerShellExe `
    -Arguments ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -InstallDir "{1}"' -f $guiPath, $InstallDir) `
    -WorkDir $InstallDir -Icon $iconShield -Description 'BIT-Web 自动登录控制面板（系统托盘）'
New-Shortcut -Path (Join-Path $menuDir '终端面板.lnk') `
    -Target $python.Python `
    -Arguments ('"{0}" --config "{1}"' -f (Join-Path $InstallDir 'tui.py'), $configPath) `
    -WorkDir $InstallDir -Icon (Join-Path $env:SystemRoot 'System32\imageres.dll,77') -Description 'BIT-Web 自动登录终端面板'
Write-Host "已创建: $menuDir"

Write-Step '注册开机自启动'
Remove-Autostart
$registered = $false
try {
    Register-LogonTask -Name $TaskName -Executable $python.PythonW -Arguments $daemonArgs `
        -Description 'BIT-Web 断线自动登录守护进程（连到 BIT-Web 时每 5 分钟检查外网并自动重新认证）'
    $registered = $true
    Write-Host '已注册守护进程计划任务（登录 Windows 后自动启动，隐藏窗口）'
} catch {
    Write-Host ('计划任务注册失败（{0}），改用启动文件夹快捷方式。' -f $_.Exception.Message) -ForegroundColor Yellow
    $lnk = Join-Path ([Environment]::GetFolderPath('Startup')) "$TaskName.lnk"
    New-Shortcut -Path $lnk -Target $python.PythonW -Arguments $daemonArgs -WorkDir $InstallDir `
        -Icon $iconShield -Description 'BIT-Web 自动登录守护进程'
    Write-Host "已创建启动快捷方式: $lnk"
}

$trayRegistered = $false
if (-not $NoTray) {
    try {
        Register-LogonTask -Name $TrayTaskName -Executable $PowerShellExe -Arguments $trayArgs `
            -Description 'BIT-Web 自动登录系统托盘控制面板'
        $trayRegistered = $true
        Write-Host '已注册系统托盘计划任务（登录 Windows 后自动出现托盘图标）'
    } catch {
        Write-Host ('托盘开机自启注册失败（{0}），可手动从开始菜单启动控制面板。' -f $_.Exception.Message) -ForegroundColor Yellow
    }
}

if (-not $NoStart) {
    Write-Step '启动守护进程与托盘面板'
    if ($registered) {
        Start-ScheduledTask -TaskName $TaskName
    } else {
        Start-Process -FilePath $python.PythonW -ArgumentList $daemonArgs -WorkingDirectory $InstallDir -WindowStyle Hidden
    }
    Start-Sleep -Seconds 2
    if ($trayRegistered) {
        Start-ScheduledTask -TaskName $TrayTaskName
    }
    Start-Sleep -Seconds 2
    if ($registered) {
        $task = Get-ScheduledTask -TaskName $TaskName
        Write-Host ("守护进程任务状态: {0}" -f $task.State)
    }
}

Write-Step '下一步'
$config = Get-Content $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
$hasPassword = $false
if ($config.PSObject.Properties.Name -contains 'password_dpapi' -and $config.password_dpapi) {
    $hasPassword = $true
}
if ($hasPassword) {
    Write-Host '密码已配置。' -ForegroundColor Green
} elseif ($NoPasswordSetup) {
    Write-Host '尚未设置密码，请运行：'
    Write-Host ('  "{0}" "{1}" --config "{2}"' -f $python.Python, (Join-Path $InstallDir 'set_password.py'), $configPath)
} else {
    Write-Host '尚未设置密码，现在开始设置（输入不会回显）：'
    & $python.Python (Join-Path $InstallDir 'set_password.py') --config $configPath
}
Write-Host ''
Write-Host '常用操作：'
Write-Host '  托盘控制面板: 开始菜单 → BIT-Web 自动登录 → 控制面板（或双击系统托盘图标）'
Write-Host '  终端面板    : 开始菜单 → BIT-Web 自动登录 → 终端面板'
Write-Host ('  查看状态    : powershell -ExecutionPolicy Bypass -File "{0}"' -f (Join-Path $InstallDir 'status.ps1'))
Write-Host ('  查看日志    : {0}' -f (Join-Path $InstallDir 'logs\bitweb_monitor.log'))
Write-Host ('  卸载        : powershell -ExecutionPolicy Bypass -File "{0}"' -f (Join-Path $InstallDir 'uninstall.ps1'))
Write-Host ''
Write-Host '安装完成。' -ForegroundColor Green
