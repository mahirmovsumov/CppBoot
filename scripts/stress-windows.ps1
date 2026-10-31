# Runs a stress test on Windows, and reports the server's memory, handles and threads over time.
# Build first with -DCPPBOOT_BUILD_BENCHMARKS=ON:
#
#   .\scripts\stress-windows.ps1 -Build build\bench -Seconds 600 [-Tls]
param(
    [string]$Build = 'build\bench',
    [int]$Seconds = 60,
    [switch]$Tls,
    [int]$Scale = 1,
    [int]$SampleSeconds = 10
)

$bin = (Resolve-Path "$Build\benchmarks").Path
$work = Join-Path $env:TEMP "cppboot-stress-$PID"
New-Item -ItemType Directory -Force $work | Out-Null

$serverArgs = @("--cppboot.logging.level=warn", "--cppboot.database.url=sqlite:$($work -replace '\\','/')/stress.db")
$clientArgs = @('--seconds', "$Seconds", '--scale', "$Scale")
if ($Tls) {
    $env:MSYS2_ARG_CONV_EXCL = '*'
    & openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$work\key.pem" -out "$work\cert.pem" -days 2 -subj '/CN=localhost' 2>$null
    $serverArgs += "--cppboot.server.tls.certificate_file=$work\cert.pem"
    $serverArgs += "--cppboot.server.tls.private_key_file=$work\key.pem"
    $clientArgs += '--tls'
}

$server = Start-Process -FilePath "$bin\cppboot-stress-server.exe" -ArgumentList $serverArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\server.out" -RedirectStandardError "$work\server.err"
$null = $server.Handle
Start-Sleep -Seconds 2
if ($server.HasExited) {
    Write-Output "The server didn't start:"
    Get-Content "$work\server.out", "$work\server.err" | Select-Object -Last 10
    exit 1
}

$client = Start-Process -FilePath "$bin\cppboot-stress-client.exe" -ArgumentList $clientArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\client.out" -RedirectStandardError "$work\client.err"
$null = $client.Handle

Write-Output 'seconds,memory_mb,handles,threads'
$start = Get-Date
while (-not $client.HasExited) {
    Start-Sleep -Seconds $SampleSeconds
    if ($server.HasExited) { break }
    $server.Refresh()
    $elapsed = [int]((Get-Date) - $start).TotalSeconds
    Write-Output ("{0},{1:N1},{2},{3}" -f $elapsed, ($server.WorkingSet64 / 1MB), $server.HandleCount, $server.Threads.Count)
}

$null = $client.WaitForExit(90000)
Get-Content "$work\client.out" | Where-Object { $_ -notmatch '^ok=' }
Get-Content "$work\client.err" | Select-Object -Last 5

$alive = -not $server.HasExited
Write-Output "server alive: $alive"
if ($alive) {
    Start-Sleep -Seconds 6
    $server.Refresh()
    Write-Output ("after the load: {0:N1} MB, {1} handles, {2} threads" -f ($server.WorkingSet64 / 1MB), $server.HandleCount, $server.Threads.Count)
    Stop-Process -Id $server.Id -Force
} else {
    Write-Output "server exit code: $($server.ExitCode)"
    Get-Content "$work\server.out", "$work\server.err" | Select-Object -Last 15
}
Get-Content "$work\server.out" | Select-String 'critical|error' | Select-Object -First 10 | ForEach-Object { "server log: $_" }

$failed = -not $alive -or $client.ExitCode -ne 0
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($failed) { exit 1 }
