#Requires -Version 5.1
<#
.SYNOPSIS
    查看原生 C 版 BIT-Web 断线自动登录的状态、任务、进程、日志，并做一次自检。
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'BITWebAutoLogin'),
    [switch]$NoSelfTest
)

$ErrorActionPreference = 'Continue'
$TaskName = 'BITWebAutoLogin'
$TrayTaskName = 'BITWebAutoLoginTray'
$cli = Join-Path $InstallDir 'bitweb-cli.exe'

Write-Host '======================================================'
Write-Host ' BIT-Web 断线自动登录 - 运行状态（原生 C 版）'
Write-Host '======================================================'

foreach ($name in @($TaskName, $TrayTaskName)) {
    $task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    if ($task) {
        $info = Get-ScheduledTaskInfo -TaskName $name
        Write-Host ('计划任务 {0}: {1}，上次运行 {2}，结果 0x{3:X}' -f $name, $task.State, $info.LastRunTime, $info.LastTaskResult)
    } else {
        Write-Host "计划任务 ${name}: 未注册"
    }
}

Get-CimInstance Win32_Process -Filter "Name = 'bitweb.exe' OR Name = 'bitweb-cli.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'bitweb' } |
    ForEach-Object { Write-Host ('进程: PID {0}  {1}' -f $_.ProcessId, $_.CommandLine) }

if (-not (Test-Path $cli)) {
    Write-Host "找不到 $cli，请先运行 install.ps1" -ForegroundColor Yellow
    exit 1
}

$oldOutput = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    Write-Host ''
    & $cli status
    if (-not $NoSelfTest) {
        Write-Host ''
        Write-Host '==> 自检'
        & $cli self-test
    }
} finally {
    [Console]::OutputEncoding = $oldOutput
}