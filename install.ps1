#Requires -Version 5.1
<#
.SYNOPSIS
    安装原生 C 版 BIT-Web 断线自动登录：守护进程 + 系统托盘控制面板 + 开机自启。

.DESCRIPTION
    - 默认安装到 %LOCALAPPDATA%\BITWebAutoLogin
    - 复制 bitweb.exe / bitweb-cli.exe、配置模板、文档
    - 注册两个「登录时启动」的计划任务：
        BITWebAutoLogin      守护进程（bitweb.exe daemon，无窗口）
        BITWebAutoLoginTray  系统托盘控制面板（bitweb.exe --silent，无窗口）
    - 不会覆盖已有的 config.json（Python 版配置可直接沿用，DPAPI 兼容）
    - 不需要 Python、不需要管理员权限

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'BITWebAutoLogin'),
    [switch]$NoStart,
    [switch]$NoTray
)

$ErrorActionPreference = 'Stop'
$TaskName = 'BITWebAutoLogin'
$TrayTaskName = 'BITWebAutoLoginTray'
$SourceDir = $PSScriptRoot
$Identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "==> $Text" -ForegroundColor Cyan
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
    Get-CimInstance Win32_Process -Filter "Name = 'bitweb.exe' OR Name = 'bitweb-cli.exe' OR Name = 'pythonw.exe' OR Name = 'python.exe' OR Name = 'powershell.exe'" -ErrorAction SilentlyContinue |
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
    Stop-MatchingProcess 'bitweb\.exe'
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
Write-Host ' BIT-Web 断线自动登录 - 安装程序（原生 C 版）'
Write-Host '======================================================'
Write-Host "安装目录: $InstallDir"

Write-Step '检查构建产物'
$guiExe = Join-Path $SourceDir 'bin\bitweb.exe'
$cliExe = Join-Path $SourceDir 'bin\bitweb-cli.exe'
if (-not (Test-Path $guiExe) -or -not (Test-Path $cliExe)) {
    Write-Host '未找到 bin\bitweb.exe / bitweb-cli.exe。' -ForegroundColor Red
    Write-Host '请先运行 build.ps1 构建（需要 MSYS2 UCRT64 gcc），或从 GitHub Releases 下载预编译版本。'
    exit 1
}

Write-Step '停止旧实例并复制程序文件'
Remove-Autostart
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item $guiExe (Join-Path $InstallDir 'bitweb.exe') -Force
Copy-Item $cliExe (Join-Path $InstallDir 'bitweb-cli.exe') -Force
foreach ($file in @('README.md', 'README.txt', 'LICENSE', 'status.ps1', 'uninstall.ps1', 'install.ps1')) {
    $src = Join-Path $SourceDir $file
    if (Test-Path $src) { Copy-Item $src (Join-Path $InstallDir $file) -Force }
}
$configPath = Join-Path $InstallDir 'config.json'
if (-not (Test-Path $configPath)) {
    Copy-Item (Join-Path $SourceDir 'config.example.json') $configPath -Force
    Write-Host '已生成配置文件 config.json'
} else {
    Write-Host '已存在 config.json，保留原有配置（Python 版配置可直接沿用）'
}
Copy-Item (Join-Path $SourceDir 'config.example.json') $InstallDir -Force

# 清理旧版 Python 运行文件（配置/日志保留）
$legacy = @('bitweb_monitor.py', 'srun_client.py', 'appconfig.py', 'dpapi.py',
            'set_password.py', 'status_report.py', 'tui.py', 'gui.ps1')
foreach ($file in $legacy) {
    $p = Join-Path $InstallDir $file
    if (Test-Path $p) { Remove-Item $p -Force }
}
New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'logs') | Out-Null

Write-Step '创建开始菜单快捷方式'
$programs = [Environment]::GetFolderPath('Programs')
$menuDir = Join-Path $programs 'BIT-Web 自动登录'
New-Item -ItemType Directory -Force -Path $menuDir | Out-Null
$icon = Join-Path $env:SystemRoot 'System32\imageres.dll,78'
New-Shortcut -Path (Join-Path $menuDir '控制面板.lnk') `
    -Target (Join-Path $InstallDir 'bitweb.exe') -Arguments '--silent' `
    -WorkDir $InstallDir -Icon $icon -Description 'BIT-Web 自动登录控制面板（系统托盘）'
New-Shortcut -Path (Join-Path $menuDir '终端面板.lnk') `
    -Target (Join-Path $InstallDir 'bitweb-cli.exe') -Arguments 'tui' `
    -WorkDir $InstallDir -Icon (Join-Path $env:SystemRoot 'System32\imageres.dll,77') -Description 'BIT-Web 自动登录终端面板'
New-Shortcut -Path (Join-Path $menuDir '自检.lnk') `
    -Target (Join-Path $InstallDir 'bitweb-cli.exe') -Arguments 'self-test' `
    -WorkDir $InstallDir -Icon $icon -Description 'BIT-Web 自动登录自检'
Write-Host "已创建: $menuDir"

Write-Step '注册开机自启动'
$registered = $false
try {
    Register-LogonTask -Name $TaskName -Executable (Join-Path $InstallDir 'bitweb.exe') `
        -Arguments 'daemon' `
        -Description 'BIT-Web 断线自动登录守护进程（原生 C，无窗口）'
    $registered = $true
    Write-Host '已注册守护进程计划任务'
} catch {
    Write-Host ('计划任务注册失败（{0}），改用启动文件夹快捷方式。' -f $_.Exception.Message) -ForegroundColor Yellow
    $lnk = Join-Path ([Environment]::GetFolderPath('Startup')) "$TaskName.lnk"
    New-Shortcut -Path $lnk -Target (Join-Path $InstallDir 'bitweb.exe') -Arguments 'daemon' `
        -WorkDir $InstallDir -Icon $icon -Description 'BIT-Web 自动登录守护进程'
    Write-Host "已创建启动快捷方式: $lnk"
}

$trayRegistered = $false
if (-not $NoTray) {
    try {
        Register-LogonTask -Name $TrayTaskName -Executable (Join-Path $InstallDir 'bitweb.exe') `
            -Arguments '--silent' `
            -Description 'BIT-Web 自动登录系统托盘控制面板（原生 C）'
        $trayRegistered = $true
        Write-Host '已注册系统托盘计划任务'
    } catch {
        Write-Host ('托盘计划任务注册失败（{0}）' -f $_.Exception.Message) -ForegroundColor Yellow
    }
}

if (-not $NoStart) {
    Write-Step '启动守护进程与托盘面板'
    if ($registered) { Start-ScheduledTask -TaskName $TaskName }
    else { Start-Process -FilePath (Join-Path $InstallDir 'bitweb.exe') -ArgumentList 'daemon' -WorkingDirectory $InstallDir }
    Start-Sleep -Seconds 2
    if ($trayRegistered) { Start-ScheduledTask -TaskName $TrayTaskName }
    Start-Sleep -Seconds 2
    if ($registered) {
        Write-Host ('守护进程任务状态: {0}' -f (Get-ScheduledTask -TaskName $TaskName).State)
    }
}

Write-Step '下一步'
$config = Get-Content $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
$hasPassword = $false
if ($config.PSObject.Properties.Name -contains 'password_dpapi' -and $config.password_dpapi) { $hasPassword = $true }
if ($hasPassword) {
    Write-Host '密码已配置（与原 Python 版通用）。' -ForegroundColor Green
} else {
    Write-Host '尚未设置密码，请运行：'
    Write-Host ('  "{0}" set-password' -f (Join-Path $InstallDir 'bitweb-cli.exe'))
}
Write-Host ''
Write-Host '常用操作：'
Write-Host '  托盘控制面板: 开始菜单 → BIT-Web 自动登录 → 控制面板（或双击托盘图标）'
Write-Host '  终端面板    : 开始菜单 → BIT-Web 自动登录 → 终端面板'
Write-Host ('  命令行状态  : "{0}" status' -f (Join-Path $InstallDir 'bitweb-cli.exe'))
Write-Host ('  自检        : "{0}" self-test' -f (Join-Path $InstallDir 'bitweb-cli.exe'))
Write-Host ('  查看日志    : {0}' -f (Join-Path $InstallDir 'logs\bitweb_monitor.log'))
Write-Host ('  卸载        : powershell -ExecutionPolicy Bypass -File "{0}"' -f (Join-Path $InstallDir 'uninstall.ps1'))
Write-Host ''
Write-Host '安装完成。' -ForegroundColor Green