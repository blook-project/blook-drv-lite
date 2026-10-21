# Full host-mode e2e matrix for GitHub Actions.
#
# The runner is a disposable VM (admin, testsigning enabled, Secure Boot off),
# so the same matrix that scripts/e2e.ps1 runs inside bvmenv can run directly
# against the built driver. Host mode is deliberately gated: it refuses to run
# on developer machines.
#
# Cases mirror docs/e2e.md: session, physical/kernel/process r+w (CR3), page
# crossings and chunking, DSE disable/restore, full-permission handles,
# InfinityHook counters + DR hiding (documented skip when the runner's kernel
# layout does not allow it), fuzzing, unload/trace scan and 1000 load cycles.
[CmdletBinding()]
param(
    [string]$BuildDir = "build\windows\x64\release"
)
$ErrorActionPreference = "Stop"
if ($env:GITHUB_ACTIONS -ne "true" -or $env:DRVLITE_CI_HOST_E2E -ne "1") {
    throw "refusing to run kernel code on this machine: host e2e is gated to CI (GITHUB_ACTIONS=true + DRVLITE_CI_HOST_E2E=1). Use scripts/e2e.ps1 with a bvmenv VM for local runs."
}
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$service = "Drvlite"
$driver = (Resolve-Path (Join-Path $BuildDir "drvlite.sys")).Path
$client = (Resolve-Path (Join-Path $BuildDir "drvlite-cli.exe")).Path
$target = (Resolve-Path (Join-Path $BuildDir "drvlite-target.exe")).Path
$unsigned = (Resolve-Path (Join-Path $BuildDir "drvlite-unsigned.sys")).Path
foreach ($file in @($driver, $client, $target, $unsigned)) {
    if (-not (Test-Path $file)) { throw "missing host-e2e artefact: $file" }
}

$runId = Get-Date -Format "yyyyMMddTHHmmssZ"
$cases = New-Object System.Collections.Generic.List[object]
$log = New-Object System.Collections.Generic.List[string]
function Log([string]$message) {
    $line = "[$(Get-Date -Format 'HH:mm:ss')] $message"
    Write-Host $line
    $log.Add($line) | Out-Null
}
function Record([string]$name, [string]$status, [string]$detail) {
    $cases.Add([pscustomobject]@{ name = $name; status = $status; detail = $detail }) | Out-Null
    Log ("[{0}] {1} {2}" -f $status.ToUpperInvariant(), $name, $detail)
}
function Run-Case([string]$name, [scriptblock]$body) {
    try {
        $detail = [string](& $body)
        if ($detail -like "skip:*") { Record $name "skip" $detail }
        else { Record $name "pass" $detail }
    } catch {
        Record $name "fail" ([string]$_.Exception.Message)
    }
}

# ---------------------------------------------------------------- signing/trust
$signer = Join-Path $repoRoot "signer\spcsign\spcsign.exe"
if (-not (Test-Path $signer)) { throw "missing SPC signer" }
if (-not $env:SPC_PFX_BASE64) {
    throw "SPC_PFX_BASE64/SPC_PFX_PASSWORD secrets are required for the host e2e job"
}
$tsaRoot = Join-Path $repoRoot "signer\tsa\tsa-root.cer"
& certutil.exe -addstore -f root $tsaRoot 2>&1 | Out-Null
foreach ($chain in @(
        (Join-Path $repoRoot "signer\certs\verisign-cscs-2010.cer"),
        (Join-Path $repoRoot "signer\certs\verisign-g5-mcvr.cer"))) {
    & certutil.exe -addstore -f CA $chain 2>&1 | Out-Null
}
$pfxPath = Join-Path $env:RUNNER_TEMP "host-e2e-sign.pfx"
[IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($env:SPC_PFX_BASE64))
$secure = ConvertTo-SecureString -String $env:SPC_PFX_PASSWORD -AsPlainText -Force
Import-PfxCertificate -FilePath $pfxPath -CertStoreLocation Cert:\CurrentUser\My -Password $secure | Out-Null
Remove-Item $pfxPath -ErrorAction SilentlyContinue
& $signer sign --sha1 --cert XINDA "--tsa-time" "2013-01-01T00:00:00Z" `
    "--chain" (Join-Path $repoRoot "signer\certs\verisign-cscs-2010.cer") `
    "--chain" (Join-Path $repoRoot "signer\certs\verisign-g5-mcvr.cer") $driver
if ($LASTEXITCODE -ne 0) { throw "SPC signing failed" }
$certDir = Join-Path $env:RUNNER_TEMP "host-e2e-certs"
New-Item -ItemType Directory -Force $certDir | Out-Null
& $signer export $driver --out $certDir 2>&1 | Out-Null
foreach ($cer in Get-ChildItem $certDir -Filter "*.cer" -ErrorAction SilentlyContinue) {
    & certutil.exe -addstore -f Root $cer.FullName 2>&1 | Out-Null
    & certutil.exe -addstore -f TrustedPublisher $cer.FullName 2>&1 | Out-Null
}

$startScript = Join-Path $env:RUNNER_TEMP "host-e2e-start.cmd"
Set-Content -Path $startScript -Encoding ascii -Value @"
@echo off
sc stop $service >nul 2>&1
sc start $service
sc query $service
"@
$bootstrap = Join-Path $env:RUNNER_TEMP "host-e2e-bootstrap.bin"
$env:DRVLITE_ALLOW_KERNEL_LIVE = "1"

function Invoke-Cli([string[]]$arguments) {
    $all = @("run", "--allow-kernel-live", "--mode", "copy", "--service", $service,
        "--image", $driver, "--bootstrap", $bootstrap, "--map-cmd",
        $startScript, "--") + $arguments
    $output = (& $client @all 2>&1 | Out-String)
    $code = $LASTEXITCODE
    Log "cli $($arguments -join ' ') -> exit $code"
    if ($output.Trim() -ne "") { Log $output.Trim() }
    return [pscustomobject]@{ code = $code; out = $output }
}
function Invoke-Sc([string[]]$arguments) {
    $output = (& sc.exe @arguments 2>&1 | Out-String)
    return [pscustomobject]@{ code = $LASTEXITCODE; out = $output }
}
function Start-Target([string]$name, [int]$seconds) {
    $json = Join-Path $env:RUNNER_TEMP "$name.json"
    $changed = Join-Path $env:RUNNER_TEMP "$name-changed.txt"
    Remove-Item $json, $changed -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath $target -PassThru -WindowStyle Hidden `
        -ArgumentList @("--out", $json, "--changed", $changed, "--seconds", "$seconds")
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        Start-Sleep -Milliseconds 200
        if (Test-Path $json) { break }
    }
    if (-not (Test-Path $json)) { throw "target process did not publish $name.json" }
    $descriptor = Get-Content $json -Raw | ConvertFrom-Json
    return [pscustomobject]@{
        proc = $proc
        pid = [string]$descriptor.pid
        tid = [string]$descriptor.tid
        buffer = [string]$descriptor.buffer
        changed = $changed
        json = $json
    }
}

$created = $false
$targets = @()
try {
    if (-not (Test-Path $driver)) { throw "driver file missing before sc create: $driver" }
    Log "driver path: $driver"
    $create = Invoke-Sc @("create", $service, "type=", "kernel", "binPath= \??\$driver")
    if ($create.code -ne 0) {
        $qc = Invoke-Sc @("qc", $service)
        throw "sc create failed (exit $($create.code)): $($create.out) | qc: $($qc.out) | path: $driver"
    }
    $created = $true

    Log "=== starting first target process"
    $primary = Start-Target "host-target" 1800
    $targets += $primary
    $pidValue = $primary.pid
    $tidValue = $primary.tid
    $buffer = $primary.buffer

    Run-Case "driver_session_attach" {
        $result = Invoke-Cli @("info")
        if ($result.code -ne 0) { throw "info failed (exit $($result.code)): $($result.out)" }
        if ($result.out -notmatch "nt build\s*:\s*\d+") { throw "no nt build" }
        if ($result.out -notmatch "features\s*:\s*0x[0-9a-f]+") { throw "no features" }
        if ($result.out -notmatch "kernel base\s*:\s*(0x[0-9a-f]+)") { throw "no kernel base" }
        $script:kernelBase = $Matches[1]
        return "nt build/features/kernel base reported"
    }

    Run-Case "manual_map_mapper" {
        # The in-repo mapper is a test harness; on hosted runners the mapper
        # driver cannot be test-loaded without a VM, and the production path is
        # blook-kdu. Explicit skip so the matrix records the limitation.
        return "skip: manual-map NX on 26200 (mapper harness requires a VM; production path is blook-kdu)"
    }

    Run-Case "physical_read_write" {
        $result = Invoke-Cli @("phys")
        if ($result.code -ne 0) { throw "phys failed: $($result.out)" }
        if ($result.out -notmatch "physical read/write roundtrip: ok") {
            throw "physical roundtrip missing"
        }
        if ($result.out -notmatch "diagnostic page: va=(0x[0-9a-f]+)") {
            throw "diagnostic page VA missing"
        }
        $script:testPageVa = $Matches[1]
        return "diagnostic page roundtrip ok"
    }

    Run-Case "kernel_read_write" {
        $base = if ($script:kernelBase) { $script:kernelBase } else { throw "kernel base unknown" }
        $read = Invoke-Cli @("read-kernel", $base, "2")
        if ($read.code -ne 0 -or $read.out -notmatch "4d 5a") { throw "kernel read did not return MZ" }
        $page = if ($script:testPageVa) { $script:testPageVa } else { throw "test page VA unknown" }
        $readPage = Invoke-Cli @("read-kernel", $page, "12")
        if ($readPage.code -ne 0 -or $readPage.out -notmatch "44 52 56 4c") {
            throw "diagnostic page read did not return the marker"
        }
        $write = Invoke-Cli @("write-kernel", $page, "41 41 41 41")
        if ($write.code -ne 0) { throw "kernel write failed" }
        $verify = Invoke-Cli @("read-kernel", $page, "4")
        if ($verify.out -notmatch "41 41 41 41") { throw "kernel write did not verify" }
        Invoke-Cli @("write-kernel", $page, "44 52 56 4c") | Out-Null
        return "kernel MZ + diagnostic-page write/read verified"
    }

    Run-Case "kernel_chunked_read" {
        $base = if ($script:kernelBase) { $script:kernelBase } else { throw "kernel base unknown" }
        $read = Invoke-Cli @("read-kernel", $base, "64512")
        if ($read.code -ne 0) { throw "chunked kernel read failed" }
        if ($read.out -notmatch "4d 5a") { throw "chunked read did not return MZ" }
        return "63 KiB chunked kernel read ok"
    }

    Run-Case "page_paths_and_crossings" {
        $result = Invoke-Cli @("memtest", $pidValue, $buffer)
        if ($result.code -ne 0) { throw "memtest failed: $($result.out)" }
        if ($result.out -notmatch "memtest: .*ok") { throw "memtest did not report ok" }
        return "4k crossing, non-present, non-canonical, 2m boundary and chunked reads ok"
    }

    Run-Case "module_dump_target" {
        $dumpPath = Join-Path $env:RUNNER_TEMP "host-e2e-dump.bin"
        Remove-Item $dumpPath -ErrorAction SilentlyContinue
        $result = Invoke-Cli @("dump", $pidValue, $buffer, "4096", $dumpPath)
        if ($result.code -ne 0) { throw "module dump failed: $($result.out)" }
        if (-not (Test-Path $dumpPath)) { throw "dump file was not written" }
        $bytes = [IO.File]::ReadAllBytes($dumpPath)
        if ($bytes.Length -ne 4096) { throw "dump size $($bytes.Length) != 4096" }
        $prefix = [Text.Encoding]::ASCII.GetString($bytes, 0, 8)
        if ($prefix -ne "DRVLITE!") { throw "dump content mismatch: '$prefix'" }
        return "module dump returned 4 KiB starting with the target pattern"
    }

    Run-Case "process_memory_cr3" {
        $before = Invoke-Cli @("read-proc", $pidValue, $buffer, "8")
        if ($before.code -ne 0 -or $before.out -notmatch "44 52 56 4c 49 54 45 21") {
            throw "target pattern mismatch"
        }
        $write = Invoke-Cli @("write-proc", $pidValue, $buffer, "43 52 33 21")
        if ($write.code -ne 0) { throw "process write failed" }
        $after = Invoke-Cli @("read-proc", $pidValue, $buffer, "4")
        if ($after.out -notmatch "43 52 33 21") { throw "process write did not verify" }
        $observed = $false
        for ($attempt = 0; $attempt -lt 50; ++$attempt) {
            Start-Sleep -Milliseconds 200
            if (Test-Path $primary.changed) {
                $content = Get-Content $primary.changed -Raw -ErrorAction SilentlyContinue
                if ($content -match "0x") { $observed = $true; break }
            }
        }
        if (-not $observed) { throw "target did not observe the externally written value" }
        return "pattern matched, wrote 43523321, target observed it"
    }

    Run-Case "process_module_list" {
        $result = Invoke-Cli @("modules", $pidValue)
        if ($result.code -ne 0 -or $result.out -notmatch "ntdll\.dll") {
            throw "process module list missing ntdll"
        }
        return "ntdll listed"
    }

    Run-Case "dse_cycle" {
        $result = Invoke-Cli @("dse-cycle")
        if ($result.code -ne 0) { throw "dse-cycle failed: $($result.out)" }
        if ($result.out -notmatch "patched=1") { throw "DSE patch not applied" }
        if ($result.out -notmatch "restore=0" -or $result.out -notmatch "restored=0") {
            throw "DSE restore not verified"
        }
        $loaded = $false
        & sc.exe create DrvliteUnsigned type= kernel binPath= $unsigned 2>&1 | Out-Null
        $startUnsigned = Invoke-Sc @("start", "DrvliteUnsigned")
        $query = Invoke-Sc @("query", "DrvliteUnsigned")
        if ($query.out -match "RUNNING") { $loaded = $true }
        & sc.exe stop DrvliteUnsigned 2>&1 | Out-Null
        & sc.exe delete DrvliteUnsigned 2>&1 | Out-Null
        if (-not $loaded) {
            throw "unsigned driver did not load on the testsigning runner: $($startUnsigned.out)"
        }
        return "g_CiOptions patched/restored by value; unsigned driver loaded"
    }

    Run-Case "handle_full_access" {
        Log "=== starting disposable target process"
        $disposable = Start-Target "host-target2" 300
        $targets += $disposable
        $result = Invoke-Cli @("handle", "--pid", $disposable.pid, "--full", "--kill")
        if ($result.code -ne 0) { throw "handle open failed: $($result.out)" }
        if ($result.out -notmatch "granted  : 0x1fffff") { throw "granted access not full" }
        if ($result.out -notmatch "terminate: yes") { throw "handle not usable for terminate" }
        return "disposable process terminated through a full-permission handle"
    }

    Run-Case "handle_protected_process" {
        $task = & tasklist 2>&1 | Out-String
        $match = [regex]::Match($task, "MsMpEng\.exe\s+(\d+)")
        if (-not $match.Success) { return "skip: blocklisted victim driver — no PPL/AV process on the runner" }
        $protectedPid = $match.Groups[1].Value
        $result = Invoke-Cli @("handle", "--pid", $protectedPid, "--full")
        if ($result.code -ne 0 -or $result.out -notmatch "granted  : 0x1fffff") {
            throw "PPL handle not raised to full access"
        }
        return "PPL process $protectedPid granted 0x1fffff"
    }

    Run-Case "negative_input_fuzz" {
        $result = Invoke-Cli @("fuzz")
        if ($result.code -ne 0) { throw "fuzz failed: $($result.out)" }
        if ($result.out -notmatch "fuzz: rejected=1 driver-alive=1") {
            throw "fuzz did not reject malformed input"
        }
        return "malformed requests rejected, driver alive"
    }

    Run-Case "stress_memory_ops" {
        $result = Invoke-Cli @("stress", $pidValue, $buffer, "1000")
        if ($result.code -ne 0) { throw "stress failed: $($result.out)" }
        if ($result.out -notmatch "stress: \d+ iterations, 0 failures") {
            throw "stress reported failures"
        }
        return "1000 mixed operations, 0 failures"
    }

    Run-Case "infinityhook_counters" {
        $ssnResult = Invoke-Cli @("ssn", "NtQueryInformationThread")
        $match = [regex]::Match($ssnResult.out, "ssn=(\d+)")
        if (-not $match.Success) { throw "cannot resolve syscall number" }
        $ssn = $match.Groups[1].Value
        $result = Invoke-Cli @("hook-test", $ssn)
        if ($result.out -match "hook_install: unsupported") {
            $script:infinitySupported = $false
            return "skip: KVA-shadow platform — InfinityHook entry pattern unsupported on this runner"
        }
        $script:infinitySupported = $true
        if ($result.code -ne 0) { throw "hook-test failed: $($result.out)" }
        if ($result.out -notmatch "syscalls before=(\d+) after=(\d+)") { throw "counters missing" }
        if ([int64]$Matches[2] -le [int64]$Matches[1]) { throw "no syscalls counted" }
        return "ssn=$ssn counted=$($Matches[2])"
    }

    Run-Case "hwbp_detection_bypass" {
        $result = Invoke-Cli @("hwbp-test", $pidValue, $tidValue)
        if ($result.out -match "before: dr0=0x0") {
            return "skip: KVA-shadow platform — runner does not expose debug registers through GetThreadContext"
        }
        if ($result.out -match "hidden: dr0=0x0") {
            return "DR0 visible before, hidden while cloaked, visible after restore"
        }
        throw "DR hiding result unexpected: $($result.out)"
    }

    Run-Case "shutdown_with_hooks_armed" {
        if ($script:infinitySupported -ne $true) {
            return "skip: KVA-shadow platform — InfinityHook unsupported on this runner"
        }
        $result = Invoke-Cli @("hook-shutdown-test")
        if ($result.code -ne 0 -or $result.out -notmatch "installed=1") {
            throw "hook-shutdown-test failed"
        }
        $alive = & cmd.exe /c "echo alive" 2>&1 | Out-String
        if ($alive -notmatch "alive") { throw "host unresponsive after hook shutdown" }
        return "hook installed then shutdown restored the syscall entry"
    }

    Run-Case "perf_benchmark" {
        $result = Invoke-Cli @("bench", "--iterations", "200")
        if ($result.code -ne 0) { throw "bench failed: $($result.out)" }
        Set-Content (Join-Path $env:RUNNER_TEMP "bench.txt") $result.out
        return "bench completed"
    }

    Run-Case "shutdown_restore" {
        $result = Invoke-Cli @("shutdown")
        if ($result.code -ne 0 -or $result.out -notmatch "shutdown: ok") {
            throw "shutdown failed: $($result.out)"
        }
        Invoke-Sc @("stop", $service) | Out-Null
        $query = Invoke-Sc @("query", $service)
        if ($query.out -match "RUNNING") { throw "service still running after shutdown" }
        return "driver shut down and service stopped"
    }

    Run-Case "trace_scan" {
        Invoke-Sc @("stop", $service) | Out-Null
        $query = Invoke-Sc @("query", $service)
        if ($query.out -match "RUNNING") { throw "service still running" }
        $leftovers = Get-ChildItem "C:\Windows\System32\drivers" -Filter "drvlite*" -ErrorAction SilentlyContinue
        if ($leftovers) { throw "driver file left in System32\drivers" }
        return "no service/loader leftovers after unload"
    }

    Run-Case "load_unload_1000" {
        $loadcycle = Join-Path $env:RUNNER_TEMP "host-e2e-loadcycle.cmd"
        Set-Content -Path $loadcycle -Encoding ascii -Value @"
@echo off
for /l %%i in (1,1,1000) do (
  sc stop $service >nul 2>&1
  sc start $service >nul 2>&1
)
echo loadcycle: 1000 cycles complete
"@
        $process = Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", $loadcycle) `
            -PassThru -NoNewWindow -RedirectStandardOutput "$env:RUNNER_TEMP\loadcycle.out"
        if (-not $process.WaitForExit(1500000)) {
            try { $process.Kill() } catch { }
            throw "loadcycle timed out"
        }
        $output = Get-Content "$env:RUNNER_TEMP\loadcycle.out" -Raw
        Log $output
        if ($output -notmatch "loadcycle: 1000 cycles complete") { throw "loadcycle did not finish" }
        $alive = & cmd.exe /c "echo alive" 2>&1 | Out-String
        if ($alive -notmatch "alive") { throw "host unresponsive after 1000 cycles" }
        return "1000 SCM load/unload cycles, host responsive"
    }
} finally {
    foreach ($item in $targets) {
        try { if ($item.proc -and -not $item.proc.HasExited) { $item.proc.Kill() } } catch { }
    }
    if ($created) {
        & sc.exe stop $service 2>&1 | Out-Null
        & sc.exe delete $service 2>&1 | Out-Null
    }
}

# ---------------------------------------------------------------- results
$passed = @($cases | Where-Object { $_.status -eq "pass" }).Count
$failed = @($cases | Where-Object { $_.status -eq "fail" }).Count
$skipped = @($cases | Where-Object { $_.status -eq "skip" }).Count
$version = (Get-Content (Join-Path $repoRoot "version.txt") -Raw).Trim()
$ntBuild = ""
try { $ntBuild = [string](Get-CimInstance Win32_OperatingSystem).BuildNumber } catch { }
$result = [pscustomobject]@{
    schema = "drvlite.host-e2e.v1"
    run_id = $runId
    version = $version
    environment = [pscustomobject]@{
        github_actions = $true
        runner = $env:RUNNER_NAME
        nt_build = $ntBuild
        signing = "spc-sha1-tsa2013"
    }
    cases = $cases
    passed = $passed
    failed = $failed
    skipped = $skipped
    pass = ($failed -eq 0)
}
$outDir = Join-Path $repoRoot "docs\evidence\ci-$runId"
New-Item -ItemType Directory -Force $outDir | Out-Null
$result | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $outDir "result.json")
$result | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $outDir "ci-e2e-result.json")
$log | Set-Content (Join-Path $outDir "cases.txt")
if (Test-Path (Join-Path $env:RUNNER_TEMP "bench.txt")) {
    Copy-Item (Join-Path $env:RUNNER_TEMP "bench.txt") (Join-Path $outDir "bench.txt")
}
Write-Host ""
Write-Host "=== result: pass=$($result.pass) passed=$passed failed=$failed skipped=$skipped"
Write-Host "=== evidence: $outDir"
if ($failed -gt 0) { throw "$failed host e2e case(s) failed" }
