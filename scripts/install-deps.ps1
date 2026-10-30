# Installs the compiler, build tools and libraries into an existing MSYS2 installation.
# Install MSYS2 from https://www.msys2.org first, using the folder D:\msys64.
param([string]$Msys2Root = 'D:\msys64')

$bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) {
    throw "MSYS2 was not found at $Msys2Root. Install it from https://www.msys2.org first."
}

$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'

# The first update can replace MSYS2's core and end the shell early, so it runs twice.
& $bash -lc 'pacman -Syu --noconfirm'
& $bash -lc 'pacman -Syu --noconfirm'

$packages = 'gcc', 'clang', 'cmake', 'ninja', 'gdb', 'yaml-cpp', 'spdlog', 'gtest', 'boost', 'nlohmann-json' |
    ForEach-Object { "mingw-w64-ucrt-x86_64-$_" }
& $bash -lc "pacman -S --needed --noconfirm $($packages -join ' ')"
if ($LASTEXITCODE -ne 0) { throw "pacman failed with exit code $LASTEXITCODE" }

Write-Host 'Done. Next, run: . .\scripts\dev-env.ps1'
