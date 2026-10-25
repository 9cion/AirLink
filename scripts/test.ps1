$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    $python = Join-Path $projectRoot '.tools\venv\Scripts\python.exe'
    & $python -m unittest discover -s tests -v
    if ($LASTEXITCODE -ne 0) { throw 'Transporttests fehlgeschlagen.' }
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $projectRoot '.tools\zig-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $projectRoot 'build\zig-cache'
    & .tools/zig-x86_64-windows-0.14.1/zig.exe cc -std=c11 -Wall -Wextra -Werror -Wno-unused-function -DUNICODE -O2 tests/language_smoke.c src/audio.c -o build/language-smoke.exe -lcomctl32 -lshell32 -lole32 -luuid -ldwmapi -luxtheme -lws2_32 -lbcrypt -lgdi32
    if ($LASTEXITCODE -ne 0) { throw 'Sprach-Testbuild fehlgeschlagen.' }
    & build/language-smoke.exe
    if ($LASTEXITCODE -ne 0) { throw 'DE/EN-Umschaltung fehlgeschlagen.' }
    & .tools/zig-x86_64-windows-0.14.1/zig.exe cc -std=c11 -Wall -Wextra -Werror -O2 tests/audio_smoke.c src/audio.c -Isrc -o build/audio-smoke.exe -lole32 -luuid -lws2_32 -lbcrypt
    if ($LASTEXITCODE -ne 0) { throw 'Audio-Testbuild fehlgeschlagen.' }
    & build/audio-smoke.exe
    if ($LASTEXITCODE -ne 0) { throw 'WASAPI-Test fehlgeschlagen. Aktives Windows-Ausgabegerät erforderlich.' }
} finally { Pop-Location }
