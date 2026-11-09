#Requires -Version 5.1
<#
.SYNOPSIS
    查看 BIT-Web 断线自动登录的运行状态、当前网络、最近日志，并做一次自检。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\status.ps1
    powershell -ExecutionPolicy Bypass -File .\status.ps1 -NoSelfTest
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'BITWebAutoLogin'),
    [switch]$NoSelfTest
)

$ErrorActionPreference = 'Continue'
$TaskName = 'BITWebAutoLogin'
$TrayTaskName = 'BITWebAutoLoginTray'

function Get-MaskedName([string]$Name) {
    if (-not $Name) { return '<未填写>' }
    if ($Name.Length -le 4) { return ('*' * $Name.Length) }
    return $Name.Substring(0, 4) + ('*' * ($Name.Length - 4))
}

function Resolve-Python([string]$Root) {
    $candidates = @(
        'C:\Python310\python.exe', 'C:\Python311\python.exe', 'C:\Python312\python.exe'
    )
    $cmd = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates = @($cmd.Source) + $candidates }
    Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python3*\python.exe') -ErrorAction SilentlyContinue |
        ForEach-Object { $candidates = @($_.FullName) + $candidates }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) { return $candidate }
    }
    return $null
}

Write-Host '======================================================'
Write-Host ' BIT-Web 断线自动登录 - 运行状态'
Write-Host '======================================================'

$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($task) {
    $info = Get-ScheduledTaskInfo -TaskName $TaskName
    Write-Host ('计划任务: 已注册，状态 {0}' -f $task.State)
    Write-Host ('上次运行: {0}，结果 0x{1:X}' -f $info.LastRunTime, $info.LastTaskResult)
    Write-Host ('下次运行: {0}' -f $info.NextRunTime)
} else {
    Write-Host '计划任务: 未注册（可能使用启动文件夹方式，或尚未安装）'
}
$trayTask = Get-ScheduledTask -TaskName $TrayTaskName -ErrorAction SilentlyContinue
if ($trayTask) {
    Write-Host ('托盘任务: 已注册，状态 {0}' -f $trayTask.State)
}
$startupLnk = Join-Path ([Environment]::GetFolderPath('Startup')) "$TaskName.lnk"
if (Test-Path $startupLnk) { Write-Host "启动快捷方式: $startupLnk" }

$procs = Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe' OR Name = 'python.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine -match 'bitweb_monitor\.py' }
if ($procs) {
    foreach ($proc in $procs) {
        Write-Host ('守护进程: PID {0} 正在运行' -f $proc.ProcessId)
    }
} else {
    Write-Host '守护进程: 未在运行' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '当前无线网络:'
netsh wlan show interfaces 2>$null | Select-String -Pattern '^\s*(SSID|State|状态)\s*:' | ForEach-Object { Write-Host ('  ' + $_.Line.Trim()) }

$configPath = Join-Path $InstallDir 'config.json'
Write-Host ''
if (Test-Path $configPath) {
    $config = Get-Content $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Write-Host "配置文件: $configPath"
    Write-Host ('用户名  : {0}' -f (Get-MaskedName $config.username))
    if ($config.PSObject.Properties.Name -contains 'password_dpapi' -and $config.password_dpapi) {
        Write-Host '密码    : 已配置（DPAPI 加密）'
    } else {
        Write-Host '密码    : 未配置（运行 set_password.py）' -ForegroundColor Yellow
    }
} else {
    Write-Host "配置文件不存在: $configPath" -ForegroundColor Yellow
}

$logPath = Join-Path $InstallDir 'logs\bitweb_monitor.log'
Write-Host ''
Write-Host "最近日志（$logPath）:"
if (Test-Path $logPath) {
    Get-Content $logPath -Encoding UTF8 -Tail 25 | ForEach-Object { Write-Host ('  ' + $_) }
} else {
    Write-Host '  暂无日志'
}

if (-not $NoSelfTest) {
    $python = Resolve-Python $InstallDir
    if ($python -and (Test-Path (Join-Path $InstallDir 'bitweb_monitor.py'))) {
        Write-Host ''
        Write-Host '==> 自检'
        $oldOutput = [Console]::OutputEncoding
        $oldPythonIo = $env:PYTHONIOENCODING
        try {
            [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
            $env:PYTHONIOENCODING = 'utf-8'
            & $python (Join-Path $InstallDir 'bitweb_monitor.py') --config $configPath --self-test
        } finally {
            [Console]::OutputEncoding = $oldOutput
            $env:PYTHONIOENCODING = $oldPythonIo
        }
    }
}
