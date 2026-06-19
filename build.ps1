# build.ps1  -  MinGW C 版（三文件：main.c + decrypt.c + gui.c）
# 需要安装：mingw-w64 (x86_64-w64-mingw32-gcc)
# Run from project root（WSL）：
#   x86_64-w64-mingw32-gcc -Os -s -DUNICODE -D_UNICODE -mwindows -Wall \
#     -Wno-unused-parameter -std=c11 \
#     -o yst_unlock.exe main.c decrypt.c gui.c app.res \
#     -lshlwapi -lshell32 -lcomctl32 -lcomdlg32 -lole32
#
# 或在 Windows PowerShell 中使用本脚本（需 PATH 中有 MinGW）：
#   .\build.ps1

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutName   = "yst_unlock.exe"
$OutPath   = Join-Path $ScriptDir $OutName

$Sources = @("main.c", "decrypt.c", "gui.c", "app.res") | ForEach-Object {
    Join-Path $ScriptDir $_
}

$Libs = "-lshlwapi", "-lshell32", "-lcomctl32", "-lcomdlg32", "-lole32"

Write-Host ">>> MinGW build starting..." -ForegroundColor Cyan

$Args = @(
    "-Os", "-s",
    "-DUNICODE", "-D_UNICODE",
    "-mwindows",
    "-Wall", "-Wno-unused-parameter",
    "-std=c11",
    "-o", $OutPath
) + $Sources + $Libs

& x86_64-w64-mingw32-gcc @Args

if ($LASTEXITCODE -eq 0) {
    $size = [math]::Round((Get-Item $OutPath).Length / 1KB, 0)
    Write-Host ">>> Build succeeded!  $OutPath  (${size} KB)" -ForegroundColor Green
} else {
    Write-Host ">>> Build FAILED." -ForegroundColor Red
    exit 1
}
