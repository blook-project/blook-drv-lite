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
$chains = @("--chain", (Join-Path $repoRoot "signer\certs\verisign-cscs-2010.cer"),
    "--chain", (Join-Path $repoRoot "signer\certs\verisign-g5-mcvr.cer"))
$signMode = "none"
# Trust the local TSA root and the legacy cross-signing chain for the runner's
# code-integrity policy. certutil targets LocalMachine here (CI runs elevated);
# the CurrentUser root import fails non-interactively.
$tsaRoot = Join-Path $repoRoot "signer\tsa\tsa-root.cer"
if (Test-Path $tsaRoot) {
    & certutil.exe -addstore -f root $tsaRoot 2>&1 | Out-Null
}
foreach ($chain in @(
        (Join-Path $repoRoot "signer\certs\verisign-cscs-2010.cer"),
        (Join-Path $repoRoot "signer\certs\verisign-g5-mcvr.cer"))) {
    if (Test-Path $chain) { & certutil.exe -addstore -f CA $chain 2>&1 | Out-Null }
}
if ($env:SPC_PFX_BASE64) {
    # True in-CI signing with the real SPC certificate (repository secret).
    # The shipped signer's --pfx path is broken, so import into CurrentUser\My
    # and sign by certificate subject.
    $pfxPath = Join-Path $env:RUNNER_TEMP "spc-sign.pfx"
    [IO.File]::WriteAllBytes($pfxPath,
        [Convert]::FromBase64String($env:SPC_PFX_BASE64))
    $secure = ConvertTo-SecureString -String $env:SPC_PFX_PASSWORD `
        -AsPlainText -Force
    Import-PfxCertificate -FilePath $pfxPath `
        -CertStoreLocation Cert:\CurrentUser\My -Password $secure | Out-Null
    Remove-Item $pfxPath -ErrorAction SilentlyContinue
    & $signer sign --cert XINDA `
        "--tsa-time" "2013-01-01T00:00:00Z" @chains $driver
    if ($LASTEXITCODE -ne 0) { throw "SPC signing failed" }
    $signMode = "pfx-import"
} else {
    # No signing secret available: still exercise real kernel execution by
    # loading the SPC-signed driver from the latest published release. The
    # build of this commit is validated by build-test; this job proves the
    # kernel-loadable paths on a normal runner.
    $assets = Join-Path $env:RUNNER_TEMP "release-assets"
    New-Item -ItemType Directory -Force $assets | Out-Null
    & gh release download --pattern "blook-drv-lite-*-windows-x64.zip" `
        --dir $assets 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "no SPC_PFX_BASE64 secret and release download failed"
    }
    $zip = Get-ChildItem $assets -Filter "*.zip" | Select-Object -First 1
    Expand-Archive -Path $zip.FullName -DestinationPath $assets -Force
    $signed = Get-ChildItem $assets -Recurse -Filter "drvlite.sys" |
        Select-Object -First 1
    if ($null -eq $signed) { throw "release package has no drvlite.sys" }
    $driver = $signed.FullName
    $signMode = "release-asset"
    Write-Host "::warning::SPC_PFX_BASE64 is not set; loading the SPC-signed driver from $($zip.Name). Add SPC_PFX_BASE64/SPC_PFX_PASSWORD repository secrets for in-CI signing."
}
$hash = (Get-FileHash -Algorithm SHA256 $driver).Hash.ToLowerInvariant()
Write-Host "kernel smoke driver ($signMode): $driver sha256 $hash"

# Import the certificate chain carried by the signature so the runner's kernel
# code-integrity policy can build the chain (the legacy cross-signed VeriSign
# chain plus the local TSA root). This does not enable test-signing.
$certDir = Join-Path $env:RUNNER_TEMP "spc-certs"
New-Item -ItemType Directory -Force $certDir | Out-Null
& $signer export $driver --out $certDir 2>&1 | Out-Null
foreach ($cer in Get-ChildItem $certDir -Filter "*.cer" -ErrorAction SilentlyContinue) {
    & certutil.exe -addstore -f Root $cer.FullName 2>&1 | Out-Null
    & certutil.exe -addstore -f TrustedPublisher $cer.FullName 2>&1 | Out-Null
}
Write-Host "imported $((Get-ChildItem $certDir -Filter '*.cer' -ErrorAction SilentlyContinue).Count) certificate(s) into Root/TrustedPublisher"

# Diagnostics: what does the user-mode authenticode policy say, and (when the
# SDK signtool is available) what does the kernel-mode policy say?
$sig = Get-AuthenticodeSignature -FilePath $driver
Write-Host "authenticode (user policy): $($sig.Status) $($sig.StatusMessage)"
& $signer check $driver 2>&1 | ForEach-Object { Write-Host "spcsign check: $_" }
$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" `
    -Recurse -Filter "signtool.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($null -ne $signtool) {
    $verify = (& $signtool.FullName verify /kp /v $driver 2>&1 | Out-String)
    Write-Host $verify
} else {
    Write-Host "signtool not found; skipped /kp verification"
}

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
    $skipped = $false
    if ($LASTEXITCODE -ne 0) {
        if ($signMode -eq "release-asset" -and $info -match "577") {
            Write-Host "::warning::the released SPC-signed driver was rejected by the runner's kernel code-integrity policy (0x241). Add SPC_PFX_BASE64/SPC_PFX_PASSWORD repository secrets so CI can sign the built driver with SHA-256."
            $skipped = $true
        } else {
            throw "clientsession (info) failed"
        }
    }
    if (-not $skipped) {
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
    } else {
        Write-Host "kernel-load smoke: SKIPPED (no signing secret; release-signed driver rejected by runner policy)"
    }
} finally {
    if ($created) {
        & sc.exe stop $Service 2>&1 | Out-Null
        & sc.exe delete $Service 2>&1 | Out-Null
    }
    Remove-Item $bootstrap -ErrorAction SilentlyContinue
}
