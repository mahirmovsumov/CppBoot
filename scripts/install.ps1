# Installs the CppBoot SDK on Windows, with MSYS2 for the compiler, CMake, Ninja and libraries.
#
#   irm https://raw.githubusercontent.com/mahirmovsumov/CppBoot/main/scripts/install.ps1 | iex
#
# To choose options, download this file and run it:
#
#   .\install.ps1 -Version 0.1.0 -Msys2Root D:\msys64
#
# It installs the SDK into your user folder, and changes only your own environment: it adds the
# SDK to PATH, and sets CPPBOOT_TOOLCHAIN to MSYS2's tools, rather than adding those to PATH where
# they could take the place of other programs.
[CmdletBinding()]
param(
    # A release such as 0.1.0, or latest, which includes pre-releases.
    [string]$Version = 'latest',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'CppBoot'),
    # Where MSYS2 is, or where to install it. Found automatically if it's in a usual place.
    [string]$Msys2Root = '',
    # An SDK archive already downloaded, instead of downloading one.
    [string]$Archive = '',
    # Builds the SDK on this machine, which takes longer but always matches its compiler.
    [switch]$FromSource,
    # Doesn't install or update MSYS2's packages.
    [switch]$SkipPackages,
    # Doesn't change PATH or CPPBOOT_TOOLCHAIN for your account.
    [switch]$NoEnvironment
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Windows PowerShell 5.1, which comes with Windows, needs -UseBasicParsing, or downloads fail on a
# machine where Internet Explorer has never been set up.
$PSDefaultParameterValues['Invoke-WebRequest:UseBasicParsing'] = $true

$repository = 'mahirmovsumov/CppBoot'
$packages = 'gcc', 'clang', 'cmake', 'ninja', 'gdb', 'yaml-cpp', 'gtest', 'boost',
    'nlohmann-json', 'postgresql', 'sqlite3', 'openssl' | ForEach-Object { "mingw-w64-ucrt-x86_64-$_" }

function Step([string]$text) { Write-Host "`n==> $text" -ForegroundColor Cyan }

function Find-Msys2 {
    if ($Msys2Root) { return $Msys2Root }
    foreach ($candidate in 'C:\msys64', 'D:\msys64') {
        if (Test-Path (Join-Path $candidate 'usr\bin\bash.exe')) { return $candidate }
    }
    return 'C:\msys64'
}

function Install-Msys2([string]$root) {
    Step "Installing MSYS2 into $root"
    $installer = Join-Path $env:TEMP 'msys2-installer.exe'
    # MSYS2 publishes no checksum for this file, so its integrity rests on downloading it over HTTPS
    # from MSYS2's own releases on GitHub.
    $source = 'https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-x86_64-latest.exe'
    Invoke-WebRequest $source -OutFile $installer
    & $installer in --confirm-command --accept-messages --root ($root -replace '\\', '/') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "The MSYS2 installer failed with exit code $LASTEXITCODE." }
    Remove-Item $installer
}

function Invoke-Msys2([string]$root, [string]$command) {
    $env:MSYSTEM = 'UCRT64'
    $env:CHERE_INVOKING = '1'
    & (Join-Path $root 'usr\bin\bash.exe') -lc $command | Out-Host
    return $LASTEXITCODE
}

function Get-Release {
    $headers = @{ 'User-Agent' = 'cppboot-installer' }
    $releases = Invoke-RestMethod "https://api.github.com/repos/$repository/releases" -Headers $headers
    $published = @($releases | Where-Object { -not $_.draft })
    if ($Version -eq 'latest') {
        if ($published.Count -eq 0) { throw 'CppBoot has no releases yet. Use -FromSource.' }
        return $published[0]
    }
    $release = $published | Where-Object { $_.tag_name -eq "v$Version" } | Select-Object -First 1
    if (-not $release) { throw "There's no CppBoot release $Version." }
    return $release
}

function Get-SdkArchive([string]$work) {
    $release = Get-Release
    $number = $release.tag_name.TrimStart('v')
    $name = "cppboot-sdk-$number-windows-x64.zip"
    $asset = $release.assets | Where-Object { $_.name -eq $name }
    $sums = $release.assets | Where-Object { $_.name -eq 'SHA256SUMS' }
    if (-not $asset -or -not $sums) { throw "Release $number has no Windows SDK. Use -FromSource." }

    Step "Downloading CppBoot $number"
    $path = Join-Path $work $name
    Invoke-WebRequest $asset.browser_download_url -OutFile $path
    # Saved to a file, because a download's content can come back as bytes rather than text.
    $sums_path = Join-Path $work 'SHA256SUMS'
    Invoke-WebRequest $sums.browser_download_url -OutFile $sums_path
    $line = Get-Content $sums_path | Where-Object { $_ -match "\s\*?$([regex]::Escape($name))$" } | Select-Object -First 1
    $expected = ($line -split '\s+')[0]
    if (-not $expected -or (Get-FileHash $path -Algorithm SHA256).Hash -ne $expected.ToUpper()) {
        throw "The download doesn't match its checksum, so it wasn't installed."
    }
    return $path
}

function Install-FromSource([string]$root, [string]$work, [string]$destination) {
    $release = if ($Version -eq 'latest') { $null } else { "v$Version" }
    $source = if ($release) { "https://github.com/$repository/archive/refs/tags/$release.zip" } else { "https://github.com/$repository/archive/refs/heads/main.zip" }
    Step "Downloading the source"
    $zip = Join-Path $work 'source.zip'
    Invoke-WebRequest $source -OutFile $zip
    Expand-Archive $zip -DestinationPath $work
    $tree = (Get-ChildItem $work -Directory | Select-Object -First 1).FullName

    Step 'Building the SDK, which takes several minutes'
    $unix = { param($path) ($path -replace '\\', '/') }
    $code = Invoke-Msys2 $root "cmake -S '$(& $unix $tree)' -B '$(& $unix $tree)/build' -G Ninja -DCMAKE_BUILD_TYPE=Release -DCPPBOOT_BUILD_TESTS=OFF -DCPPBOOT_BUILD_EXAMPLES=OFF -DCPPBOOT_BUNDLE_RUNTIME=ON && cmake --build '$(& $unix $tree)/build' && cmake --install '$(& $unix $tree)/build' --prefix '$(& $unix $destination)'"
    if ($code -ne 0) { throw "Building the SDK failed with exit code $code." }
}

function Add-ToUserPath([string]$folder) {
    $current = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @($current -split ';' | Where-Object { $_ })
    if ($entries -notcontains $folder) {
        [Environment]::SetEnvironmentVariable('Path', (@($folder) + $entries) -join ';', 'User')
    }
}

# --- Main ---

$root = Find-Msys2
if (-not (Test-Path (Join-Path $root 'usr\bin\bash.exe'))) {
    Install-Msys2 $root
}
$toolchain = Join-Path $root 'ucrt64\bin'

if (-not $SkipPackages) {
    Step 'Installing the compiler, CMake, Ninja and libraries with pacman'
    # The first update can replace MSYS2's core and end the shell early, so it runs twice.
    Invoke-Msys2 $root 'pacman -Syu --noconfirm' | Out-Null
    Invoke-Msys2 $root 'pacman -Syu --noconfirm' | Out-Null
    $code = Invoke-Msys2 $root "pacman -S --needed --noconfirm $($packages -join ' ')"
    if ($code -ne 0) { throw "pacman failed with exit code $code." }
}

$work = Join-Path $env:TEMP "cppboot-install-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
New-Item -ItemType Directory -Force $InstallDir | Out-Null
$sdk = Join-Path $InstallDir 'sdk'
$staged = Join-Path $InstallDir 'sdk.new'
try {
    if (Test-Path $staged) { Remove-Item $staged -Recurse -Force }
    if ($FromSource) {
        Install-FromSource $root $work $staged
    } else {
        $path = if ($Archive) { (Resolve-Path $Archive).Path } else { Get-SdkArchive $work }
        Step 'Unpacking the SDK'
        $unpacked = Join-Path $work 'unpacked'
        Expand-Archive $path -DestinationPath $unpacked
        $top = Get-ChildItem $unpacked -Directory | Select-Object -First 1
        Move-Item $top.FullName $staged
    }
    # The new version is complete before the old one goes.
    if (Test-Path $sdk) { Remove-Item $sdk -Recurse -Force }
    Move-Item $staged $sdk
} finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

$bin = Join-Path $sdk 'bin'
if (-not $NoEnvironment) {
    Step 'Adding CppBoot to your PATH'
    Add-ToUserPath $bin
    [Environment]::SetEnvironmentVariable('CPPBOOT_TOOLCHAIN', $toolchain, 'User')
}
# This window can use it straight away, even if the account's settings weren't changed.
$env:PATH = "$bin;$env:PATH"
$env:CPPBOOT_TOOLCHAIN = $toolchain

Step 'Checking the installation'
& (Join-Path $bin 'cppboot.exe') doctor
$healthy = $LASTEXITCODE -eq 0

Write-Host ''
if ($healthy) {
    Write-Host "CppBoot is installed in $sdk." -ForegroundColor Green
    Write-Host 'Open a new terminal, then create your first project:'
    Write-Host '  cppboot create hello-world'
    Write-Host '  cd hello-world'
    Write-Host '  cppboot run'
} else {
    Write-Host 'CppBoot is installed, but cppboot doctor found problems. Fix them, then run it again.' -ForegroundColor Yellow
    exit 1
}
