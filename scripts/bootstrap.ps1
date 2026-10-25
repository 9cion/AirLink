$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force .tools | Out-Null
    $zigPath = Join-Path $projectRoot '.tools\zig-x86_64-windows-0.14.1\zig.exe'
    if (-not (Test-Path -LiteralPath $zigPath)) {
        $zip = Join-Path $projectRoot '.tools\zig.zip'
        Invoke-WebRequest -Uri 'https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip' -OutFile $zip
        $expected = '554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c'
        if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Zig-Prüfsumme stimmt nicht.' }
        Expand-Archive -LiteralPath $zip -DestinationPath .tools -Force
    }
    $python = Join-Path $projectRoot '.tools\venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $python)) {
        python -m venv .tools/venv
        if ($LASTEXITCODE -ne 0) { throw 'Python 3.13 x64 wird für den Build benötigt.' }
    }
    & $python -m pip install -r transport/requirements-lock.txt
    if ($LASTEXITCODE -ne 0) { throw 'Abhängigkeiten konnten nicht installiert werden.' }
    Write-Host 'Build-Werkzeuge bereit. Für den Installer zusätzlich Inno Setup 6 installieren oder unter .tools/Inno ablegen.'
} finally { Pop-Location }

