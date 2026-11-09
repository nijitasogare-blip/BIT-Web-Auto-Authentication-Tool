#Requires -Version 5.1
<#
.SYNOPSIS
    使用 MSYS2 MinGW-w64 (UCRT64) gcc 构建原生 C 版本。
.DESCRIPTION
    输出：bin\bitweb.exe（GUI/托盘 + daemon）、bin\bitweb-cli.exe（status/tui/self-test/set-password）
    仅依赖 Windows 系统 DLL（不依赖 Python）。
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\build.ps1
#>
[CmdletBinding()]
param(
    [string]$Gcc = '',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$BinDir = Join-Path $Root 'bin'
$ObjDir = Join-Path $BinDir 'obj'
$SrcDir = Join-Path $Root 'src'

function Resolve-Gcc {
    if ($Gcc) { return $Gcc }
    $cmd = Get-Command gcc.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @(
            'C:\msys64\ucrt64\bin\gcc.exe',
            'C:\msys64\mingw64\bin\gcc.exe',
            'C:\msys64\mingw32\bin\gcc.exe'
        )) {
        if (Test-Path $p) { return $p }
    }
    throw '未找到 gcc.exe。请安装 MSYS2（https://www.msys2.org/）并执行 pacman -S mingw-w64-ucrt-x86_64-gcc，或用 -Gcc 指定路径。'
}

$gcc = Resolve-Gcc
Write-Host "使用编译器: $gcc"
$gccDir = Split-Path $gcc -Parent
$env:Path = "$gccDir;$env:Path"

if ($Clean) {
    Remove-Item $BinDir -Recurse -Force -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $ObjDir | Out-Null

$Cflags = @(
    '-O2', '-municode', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0601',
    '-finput-charset=UTF-8', '-fexec-charset=UTF-8', '-fwide-exec-charset=UTF-16LE',
    '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-sign-compare'
)
$Common = @(
    'json.c', 'util.c', 'crypto.c', 'config.c', 'http.c',
    'wlan.c', 'net.c', 'srun.c', 'status.c'
)
$Libs = @(
    '-lwinhttp', '-lwlanapi', '-liphlpapi', '-lws2_32', '-lcrypt32',
    '-lcomctl32', '-lshell32', '-luser32', '-lgdi32', '-lole32',
    '-luxtheme', '-lshlwapi', '-ladvapi32', '-static', '-static-libgcc'
)

$objCommon = @()
foreach ($file in $Common) {
    $obj = Join-Path $ObjDir (([System.IO.Path]::GetFileNameWithoutExtension($file)) + '.o')
    Write-Host "  CC $file"
    & $gcc @Cflags -c (Join-Path $SrcDir $file) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "编译失败: $file" }
    $objCommon += $obj
}

Write-Host '  LINK bitweb.exe (GUI/托盘 + daemon)'
$guiObjs = @()
foreach ($file in @('main_gui.c', 'gui.c', 'daemon.c')) {
    $obj = Join-Path $ObjDir (([System.IO.Path]::GetFileNameWithoutExtension($file)) + '.o')
    & $gcc @Cflags -c (Join-Path $SrcDir $file) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "编译失败: $file" }
    $guiObjs += $obj
}
$allGui = $guiObjs + $objCommon
& $gcc @allGui -o (Join-Path $BinDir 'bitweb.exe') -mwindows -municode @Libs
if ($LASTEXITCODE -ne 0) { throw '链接 bitweb.exe 失败' }

Write-Host '  LINK bitweb-cli.exe (status/tui/self-test/set-password)'
$cliObjs = @()
foreach ($file in @('main_cli.c', 'tui.c')) {
    $obj = Join-Path $ObjDir (([System.IO.Path]::GetFileNameWithoutExtension($file)) + '.o')
    & $gcc @Cflags -c (Join-Path $SrcDir $file) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "编译失败: $file" }
    $cliObjs += $obj
}
$allCli = $cliObjs + $objCommon
& $gcc @allCli -o (Join-Path $BinDir 'bitweb-cli.exe') -mconsole -municode @Libs
if ($LASTEXITCODE -ne 0) { throw '链接 bitweb-cli.exe 失败' }

Write-Host ''
Get-ChildItem $BinDir -Filter '*.exe' |
    ForEach-Object { Write-Host ("构建完成: {0}  ({1:N0} KB)" -f $_.FullName, ($_.Length / 1KB)) }
