param([switch]$SkipTransport, [switch]$Installer)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    $zig = Join-Path $projectRoot '.tools\zig-x86_64-windows-0.14.1\zig.exe'
    $python = Join-Path $projectRoot '.tools\venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $zig)) { throw 'Zig fehlt. Zuerst scripts/bootstrap.ps1 ausführen.' }
    New-Item -ItemType Directory -Force build,build/release,dist | Out-Null
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $projectRoot '.tools\zig-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $projectRoot 'build\zig-cache'
    & $zig cc -std=c11 -Wall -Wextra -Werror -O2 -municode '-Wl,--subsystem,windows' src/main.c src/audio.c -o build/release/AirLink.exe -lcomctl32 -lshell32 -lole32 -luuid -ldwmapi -luxtheme -lws2_32 -lbcrypt -lgdi32
    if ($LASTEXITCODE -ne 0) { throw 'C-Build fehlgeschlagen.' }
    if (-not $SkipTransport) {
        $env:PYINSTALLER_CONFIG_DIR = Join-Path $projectRoot '.tools\pyinstaller-cache'
        & $python -m PyInstaller --noconfirm --onefile --name AirLink.Transport --distpath build/release --workpath build/transport --specpath build --collect-submodules pyatv --collect-data pyatv transport/host.py
        if ($LASTEXITCODE -ne 0) { throw 'Transport-Build fehlgeschlagen.' }
    }
    if (-not (Test-Path -LiteralPath 'build/release/AirLink.Transport.exe')) { throw 'Transport fehlt. Einmal ohne -SkipTransport bauen.' }
    & $python scripts/collect_licenses.py
    if ($LASTEXITCODE -ne 0) { throw 'Lizenzsammlung fehlgeschlagen.' }
    if ($Installer) {
        $iscc = Join-Path $projectRoot '.tools\Inno\ISCC.exe'
        if (-not (Test-Path -LiteralPath $iscc)) { $iscc = (Get-Command ISCC.exe -ErrorAction Stop).Source }
        & $iscc installer/AirLink.iss
        if ($LASTEXITCODE -ne 0) { throw 'Installer-Build fehlgeschlagen.' }
    }
} finally { Pop-Location }

