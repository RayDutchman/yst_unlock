# build.ps1
# Nuitka one-file build for main.py
# Run from project root:  .\build.ps1

$ErrorActionPreference = "Stop"

$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SourceFile = Join-Path $ScriptDir "main.py"
$OutDir     = Join-Path $ScriptDir "dist"

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

Write-Host ">>> Nuitka build starting..." -ForegroundColor Cyan
Write-Host "    Source : $SourceFile"
Write-Host "    Output : $OutDir"
Write-Host ""

python -m nuitka `
    --onefile `
    --windows-console-mode=disable `
    --output-filename=YSTUnlock.exe `
    --output-dir="$OutDir" `
    --enable-plugin=tk-inter `
    --assume-yes-for-downloads `
    --jobs=4 `
    "$SourceFile"

if ($LASTEXITCODE -eq 0) {
    $exe  = Join-Path $OutDir "YSTUnlock.exe"
    $size = [math]::Round((Get-Item $exe).Length / 1MB, 1)
    Write-Host ""
    Write-Host ">>> Build succeeded!" -ForegroundColor Green
    Write-Host "    File : $exe"
    Write-Host "    Size : ${size} MB"
} else {
    Write-Host ""
    Write-Host ">>> Build FAILED." -ForegroundColor Red
    exit 1
}
