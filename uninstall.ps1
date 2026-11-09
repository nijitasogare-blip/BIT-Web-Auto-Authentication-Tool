#Requires -Version 5.1
<#
.SYNOPSIS
    卸载 BIT-Web 断线自动登录：移除开机自启动、托盘、开始菜单快捷方式，可选删除程序与配置。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\uninstall.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\uninstall.ps1 -RemoveData
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'BITWebAutoLogin'),
    [switch]$RemoveData,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$TaskName = 'BITWebAutoLogin'
$TrayTaskName = 'BITWebAutoLoginTray'

function Stop-MatchingProcess([string]$Pattern) {
    Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe' OR Name = 'python.exe' OR Name = 'powershell.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -and $_.CommandLine -match $Pattern } |
        ForEach-Object {
            try {
                Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop
                Write-Host ("已结束进程 PID {0}" -f $_.ProcessId)
            } catch {
                Write-Host ("结束进程 {0} 失败: {1}" -f $_.ProcessId, $_.Exception.Message)
            }
        }
}

Write-Host '==> 停止并移除开机自启动'
foreach ($name in @($TaskName, $TrayTaskName)) {
    $task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
    if ($task) {
        Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $name -Confirm:$false
        Write-Host "已删除计划任务: $name"
    } else {
        Write-Host "计划任务不存在: $name"
    }
}
$startup = [Environment]::GetFolderPath('Startup')
foreach ($name in @($TaskName, $TrayTaskName)) {
    $lnkPath = Join-Path $startup "$name.lnk"
    if (Test-Path $lnkPath) {
        Remove-Item $lnkPath -Force
        Write-Host "已删除启动快捷方式: $lnkPath"
    }
}

Write-Host '==> 结束正在运行的进程'
Stop-MatchingProcess 'bitweb_monitor\.py'
Stop-MatchingProcess 'gui\.ps1'

Write-Host '==> 删除开始菜单快捷方式'
$menuDir = Join-Path ([Environment]::GetFolderPath('Programs')) 'BIT-Web 自动登录'
if (Test-Path $menuDir) {
    Remove-Item $menuDir -Recurse -Force
    Write-Host "已删除: $menuDir"
}

if ($RemoveData) {
    $full = [System.IO.Path]::GetFullPath($InstallDir)
    $expected = [System.IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'BITWebAutoLogin'))
    $allowed = ($full -eq $expected) -or $full.StartsWith($expected + [System.IO.Path]::DirectorySeparatorChar)
    if (-not $allowed) {
        Write-Host "拒绝删除非默认安装目录: $full（如确需删除请手动操作）" -ForegroundColor Red
        exit 1
    }
    if ((Test-Path $full) -and -not $Force) {
        $answer = Read-Host "将删除程序、配置和日志目录 $full，确认？(y/N)"
        if ($answer -ne 'y' -and $answer -ne 'Y') {
            Write-Host '已取消删除（自启动项已移除）。'
            exit 0
        }
    }
    if (Test-Path $full) {
        Remove-Item $full -Recurse -Force
        Write-Host "已删除目录: $full"
    }
} else {
    Write-Host '程序与配置保留（如需删除请加 -RemoveData）。'
}

Write-Host ''
Write-Host '卸载完成。' -ForegroundColor Green
