# CI kernel-load smoke test.
#
# Runs only inside GitHub Actions: the driver is signed with the repository's
# real SPC signer (no test-signing mode), installed with sc.exe, started, then
# exercised through drvlite-cli. This proves that the SCM-loadable paths
# actually execute in kernel mode on a normal windows-latest runner.
#
# Host safety: this script deliberately refuses to run outside CI. Kernel
# experiments are only allowed in CI or inside a bvmenv VM guest.
[CmdletBinding()]
param(
    [string]$BuildDir = "build\windows\x64\release",
    [string]$Service = "Drvlite"
)
$ErrorActionPreference = "Stop"
if (-not $env:GITHUB_ACTIONS) {
    throw "refusing to load kernel code outside CI (use a bvmenv VM guest for local runs)"
}
$repoRoot = Split-Path -Parent $PSScriptRoot
$driver = Join-Path $repoRoot (Join-Path $BuildDir "drvlite.sys")
$client = Join-Path $repoRoot (Join-Path $BuildDir "drvlite-cli.exe")
foreach ($path in @($driver, $client)) {
    if (-not (Test-Path $path)) { throw "missing smoke-test binary: $path" }
}

$signer = Join-Path $repoRoot "signer\spcsign\spcsign.exe"
if (-not (Test-Path $signer)) { throw "missing signer: $signer" }
$signArgs = @("sign", "--sha1", "--cert", "XINDA",
    "--tsa-time", "2013-01-01T00:00:00Z",
    "--chain", (Join-Path $repoRoot "signer\certs\verisign-cscs-2010.cer"),
    "--chain", (Join-Path $repoRoot "signer\certs\verisign-g5-mcvr.cer"),
    $driver)
& $signer @signArgs
if ($LASTEXITCODE -ne 0) { throw "SPC signing failed" }
$hash = (Get-FileHash -Algorithm SHA256 $driver).Hash.ToLowerInvariant()
Write-Host "signed drvlite.sys sha256 $hash"

$startScript = Join-Path $env:RUNNER_TEMP "smoke-start-$Service.cmd"
$bootstrap = Join-Path $env:RUNNER_TEMP "smoke-bootstrap-$Service.bin"
$startBody = "@echo off`r`nsc stop $Service >nul 2>&1`r`nsc start $Service`r`nsc query $Service`r`n"
Set-Content -Path $startScript -Value $startBody -Encoding ascii

$created = $false
try {
    & sc.exe create $Service type= kernel binPath= $driver | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc create failed" }
    $created = $true

    $env:DRVLITE_ALLOW_KERNEL_LIVE = "1"

    # First session: publish the kchannel bootstrap in the registry, start the
    # service, handshake and query the driver.
    $info = (& $client run --allow-kernel-live --mode copy `
            --service $Service --image $driver --bootstrap $bootstrap `
            --map-cmd $startScript -- info 2>&1 | Out-String)
    Write-Host $info
    if ($LASTEXITCODE -ne 0) { throw "clientsession (info) failed" }
    if ($info -notmatch "nt build\s*:\s*\d+") { throw "info did not report nt build" }
    if ($info -notmatch "features\s*:\s*0x[0-9a-f]+") { throw "info did not report features" }
    if ($info -notmatch "kernel base\s*:\s*(0x[0-9a-f]+)") { throw "info did not report kernel base" }
    $kernelBase = $Matches[1]

    $state = (& sc.exe query $Service | Out-String)
    if ($state -notmatch "RUNNING") { throw "service is not RUNNING: $state" }

    # Second session: read the MZ header through the kernel CR3 walk. This
    # proves opcode handling, not just DriverEntry.
    $read = (& $client run --allow-kernel-live --mode copy `
            --service $Service --image $driver --bootstrap $bootstrap `
            --map-cmd $startScript -- read-kernel $kernelBase 2 2>&1 | Out-String)
    Write-Host $read
    if ($LASTEXITCODE -ne 0) { throw "client session (read-kernel) failed" }
    if ($read -notmatch "4d 5a") { throw "kernel read did not return the MZ header" }

    Write-Host "kernel-load smoke: OK (driver signed with SPC, loaded by SCM, kernel read verified)"
} finally {
    if ($created) {
        & sc.exe stop $Service 2>&1 | Out-Null
        & sc.exe delete $Service 2>&1 | Out-Null
    }
    Remove-Item $bootstrap -ErrorAction SilentlyContinue
}
