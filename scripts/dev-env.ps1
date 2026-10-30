# Adds the MSYS2 UCRT64 tools to PATH for the current PowerShell session only.
# Dot-source it so the change stays in your session: . .\scripts\dev-env.ps1
param([string]$Msys2Root = 'D:\msys64')

$bin = Join-Path $Msys2Root 'ucrt64\bin'
if (-not (Test-Path (Join-Path $bin 'g++.exe'))) {
    throw "g++ was not found in $bin. Run scripts\install-deps.ps1 first."
}

$env:PATH = "$bin;$env:PATH"
g++ --version | Select-Object -First 1
cmake --version | Select-Object -First 1
