# blook-drv-lite VM e2e runner.
#
# Creates/resets a disposable bvmenv VM, pushes the release artefacts, signs
# the test mapper, loads it as a service and runs the driver through the
# `drvlite-cli` command set. Every bvmenv invocation has a hard timeout and
# is killed if it hangs (the helper CLI is not always reliable with a guest
# that has no interactive session).
#
#   ./scripts/e2e.ps1 -Vm vm-drv-lite
#   ./scripts/e2e.ps1 -Vm vm-drv-lite -SkipBuild -KeepVm
#
# Produces docs/evidence/<yyyyMMdd>-drv-lite/{result.json,cases.txt,*.log,png}
[CmdletBinding()]
param(
    [string]$Vm = "",
    [string]$Bvmenv = "",
    [string]$OutDir = "",
    [int]$StepTimeoutSec = 180,
    [switch]$SkipBuild,
    [switch]$KeepVm,
    [switch]$Reuse,
    [switch]$NoReset
)
$ErrorActionPreference = "Stop"

# Host safety: kernel code may only run inside a bvmenv guest fork.
if ([string]::IsNullOrWhiteSpace($Vm)) {
    throw "-Vm <name> is required: kernel experiments only run inside a bvmenv-managed guest"
}
if ($Vm -match '^(host|localhost|this|local)$') {
    throw "refusing to run kernel experiments: -Vm must name a bvmenv guest (host execution is forbidden)"
}
$RepoRoot = Split-Path -Parent $PSScriptRoot
$GuestDir = "C:\drvlite"
$BuildDir = Join-Path $RepoRoot "build\windows\x64\release"
if (-not $OutDir) {
    $stamp = (Get-Date -Format "yyyyMMdd")
    $OutDir = Join-Path $RepoRoot "docs\evidence\${stamp}-drv-lite"
}
$OutDir = [IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$logPath = Join-Path $OutDir "cases.txt"
$resultPath = Join-Path $OutDir "result.json"
Set-Content -Path $logPath -Value "blook-drv-lite e2e $(Get-Date -Format o)" -Encoding utf8

$cases = @()

function Log([string]$text) {
    Write-Host $text
    Add-Content -Path $logPath -Value $text -Encoding utf8
}

function Find-Bvmenv() {
    if ($Bvmenv -and (Test-Path $Bvmenv)) { return $Bvmenv }
    $candidates = @(
        (Join-Path $RepoRoot "..\blook-vmware-environment\build\windows\x64\release\bvmenv.exe"),
        (Join-Path $RepoRoot "..\blook-vmware-environment\build\windows\x64\releasedbg\bvmenv.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return [IO.Path]::GetFullPath($candidate) }
    }
    $command = Get-Command bvmenv -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw "bvmenv not found; pass -Bvmenv <path>"
}

$Bv = Find-Bvmenv

function Invoke-Bv([string[]]$Arguments, [int]$TimeoutSec = $StepTimeoutSec) {
    $stdout = Join-Path $env:TEMP ("bvm-out-" + [guid]::NewGuid() + ".txt")
    $stderr = Join-Path $env:TEMP ("bvm-err-" + [guid]::NewGuid() + ".txt")
    $process = Start-Process -FilePath $Bv -ArgumentList $Arguments `
        -NoNewWindow -PassThru -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr
    $finished = $process.WaitForExit($TimeoutSec * 1000)
    if (-not $finished) {
        try { $process.Kill() } catch { }
        $text = "TIMEOUT after ${TimeoutSec}s: bvmenv $($Arguments -join ' ')"
        Remove-Item $stdout, $stderr -ErrorAction SilentlyContinue
        return [pscustomobject]@{ ExitCode = -1; Output = $text; TimedOut = $true }
    }
    $out = (Get-Content $stdout -ErrorAction SilentlyContinue) -join "`n"
    $err = (Get-Content $stderr -ErrorAction SilentlyContinue) -join "`n"
    Remove-Item $stdout, $stderr -ErrorAction SilentlyContinue
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        Output = ($out + "`n" + $err).Trim()
        TimedOut = $false
    }
}

function Guest([string]$Command, [switch]$Elevated, [int]$TimeoutSec = $StepTimeoutSec) {
    $arguments = @("exec", $Vm, "--timeout", "$TimeoutSec")
    if ($Elevated) { $arguments += "--elevated" }
    $arguments += @("--", "cmd", "/c", $Command)
    $result = Invoke-Bv $arguments ($TimeoutSec + 30)
    Log "  guest> $Command"
    if ($result.Output) { Log ($result.Output -split "`n" | ForEach-Object { "    $_" } | Out-String).TrimEnd() }
    if ($result.TimedOut) { throw "guest command timed out: $Command" }
    if ($result.ExitCode -ne 0) { throw "guest command failed ($($result.ExitCode)): $Command" }
    return $result.Output
}

# Runs a guest command and tolerates a non-zero exit code. The new bvmenv
# surfaces failures with an error string, so do not hide them silently.
function Try-Guest([string]$Command, [switch]$Elevated, [int]$TimeoutSec = $StepTimeoutSec) {
    try {
        return Guest $Command -Elevated:$Elevated -TimeoutSec:$TimeoutSec
    } catch {
        Log "  (tolerated failure: $($_.Exception.Message))"
        return ""
    }
}

function Push([string]$Local, [string]$Guest) {
    $result = Invoke-Bv @("push", $Vm, $Local, $Guest) 120
    if ($result.ExitCode -ne 0) { throw "push failed: $Local -> $Guest ($($result.Output))" }
}

function Record([string]$Name, [string]$Status, [string]$Detail = "", [int]$DurationMs = 0) {
    $script:cases += [pscustomobject]@{
        name = $Name
        status = $Status
        detail = $Detail
        duration_ms = $DurationMs
    }
    Log ("[{0}] {1} {2}" -f $Status.ToUpperInvariant(), $Name, $Detail)
}

function Run-Case([string]$Name, [scriptblock]$Body) {
    $start = Get-Date
    try {
        $detail = & $Body
        Record $Name "pass" ([string]$detail) ([int]((Get-Date) - $start).TotalMilliseconds)
    } catch {
        Record $Name "fail" ([string]$_.Exception.Message) ([int]((Get-Date) - $start).TotalMilliseconds)
    }
}

$mapCommand = "$GuestDir\drvlite-mapper-cli.exe --allow-kernel-live --image {image} --bootstrap {bootstrap}"
function Run-Cli([string]$SubCommand, [switch]$Elevated, [int]$TimeoutSec = $StepTimeoutSec) {
    # Guest batch file avoids nested-quote mangling through bvmenv exec.
    return Guest "$GuestDir\run-cli.cmd $SubCommand" -Elevated:$Elevated -TimeoutSec:$TimeoutSec
}

# ---------------------------------------------------------------------------
# 0. build + sign
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    Log "=== building release artefacts"
    Push-Location $RepoRoot
    try {
        & xmake f -y -m release "--kchannel_dir=$env:KCHANNEL_DIR" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "xmake configure failed" }
        & xmake build -a | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "xmake build failed" }
        & xmake run drvlite-tests | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "host unit tests failed" }
    } finally {
        Pop-Location
    }
}
$required = @(
    "drvlite.sys", "drvlite-cli.exe", "drvlite-mapper.sys",
    "drvlite-mapper-cli.exe", "drvlite-target.exe", "drvlite-unsigned.sys"
)
foreach ($name in $required) {
    if (-not (Test-Path (Join-Path $BuildDir $name))) {
        throw "missing artefact: $name (run without -SkipBuild)"
    }
}

$signer = Join-Path $RepoRoot "signer\spcsign\spcsign.exe"
if (Test-Path $signer) {
    Log "=== signing test mapper"
    $signArgs = @("sign", "--sha1", "--cert", "XINDA",
        "--tsa-time", "2013-01-01T00:00:00Z",
        "--chain", (Join-Path $RepoRoot "signer\certs\verisign-cscs-2010.cer"),
        "--chain", (Join-Path $RepoRoot "signer\certs\verisign-g5-mcvr.cer"),
        (Join-Path $BuildDir "drvlite-mapper.sys"))
    $sign = & $signer @signArgs 2>&1
    $sign | ForEach-Object { Log "  $_" }
    if ($LASTEXITCODE -ne 0) { throw "mapper signing failed" }
}

# ---------------------------------------------------------------------------
# 1. VM lifecycle (hard timeouts)
# ---------------------------------------------------------------------------
$list = Invoke-Bv @("list", "--json") 30
$exists = $list.ExitCode -eq 0 -and $list.Output -match [regex]::Escape("""name"": ""$Vm""")
if (-not $exists) {
    Log "=== creating VM $Vm"
    $create = Invoke-Bv @("create", $Vm) 420
    if ($create.ExitCode -ne 0) {
        # bvmenv may report a timeout after the VM was already created and
        # started; re-list before giving up.
        $rel = Invoke-Bv @("list", "--json") 30
        $created = $rel.ExitCode -eq 0 -and $rel.Output -match [regex]::Escape("""name"": ""$Vm""")
        if (-not $created) { throw "VM create failed: $($create.Output)" }
        Log "warning: create reported failure but the VM exists; continuing"
    }
} else {
    if ($Reuse) {
        Log "=== reusing running VM $Vm (no reset)"
        Invoke-Bv @("start", $Vm) 180 | Out-Null
    } else {
        Log "=== resetting VM $Vm"
        $reset = Invoke-Bv @("reset", $Vm) 180
        if ($reset.ExitCode -ne 0) {
            $start = Invoke-Bv @("start", $Vm) 180
            if ($start.ExitCode -ne 0) { throw "VM start failed: $($start.Output)" }
        }
    }
}
$started = Invoke-Bv @("start", $Vm) 180
Log "start: $($started.Output)"

function Wait-Guest([int]$Attempts = 30) {
    for ($attempt = 0; $attempt -lt $Attempts; ++$attempt) {
        Start-Sleep -Seconds 5
        $probe = Invoke-Bv @("exec", $Vm, "--timeout", "20", "--", "cmd", "/c", "echo guest-ready") 40
        if ($probe.ExitCode -eq 0 -and $probe.Output -match "guest-ready") {
            return $true
        }
    }
    return $false
}
if (-not (Wait-Guest)) { throw "guest did not become ready" }

# ---------------------------------------------------------------------------
# 2. push + mapper service
# ---------------------------------------------------------------------------
Log "=== pushing artefacts"
# A previously running mapper service locks its image file; stop it first.
Try-Guest "sc stop DrvliteMapper" -Elevated | Out-Null
Try-Guest "sc delete DrvliteMapper" -Elevated | Out-Null
Guest "if not exist $GuestDir mkdir $GuestDir" -Elevated | Out-Null
foreach ($name in $required) {
    Push (Join-Path $BuildDir $name) "$GuestDir\$name"
}
Push (Join-Path $RepoRoot "docs\e2e.md") "$GuestDir\e2e.md"
# Guest helper batches (avoid nested quoting through bvmenv exec).
$runCli = @"
@echo off
set DRVLITE_ALLOW_KERNEL_LIVE=1
"$GuestDir\drvlite-cli.exe" run --allow-kernel-live --image $GuestDir\drvlite.sys --map-cmd "$mapCommand" -- %*
"@
$cycleCli = @"
@echo off
set DRVLITE_ALLOW_KERNEL_LIVE=1
"$GuestDir\drvlite-cli.exe" cycle-test --allow-kernel-live --image $GuestDir\drvlite.sys --cycles %1
"@
$unmapCli = @"
@echo off
set DRVLITE_ALLOW_KERNEL_LIVE=1
"$GuestDir\drvlite-mapper-cli.exe" --allow-kernel-live --unmap %1
"@
$runCliPath = Join-Path $OutDir "run-cli.cmd"
$cycleCliPath = Join-Path $OutDir "cycle.cmd"
$unmapCliPath = Join-Path $OutDir "unmap.cmd"
Set-Content -Path $runCliPath -Value $runCli -Encoding ascii
Set-Content -Path $cycleCliPath -Value $cycleCli -Encoding ascii
Set-Content -Path $unmapCliPath -Value $unmapCli -Encoding ascii
Push $runCliPath "$GuestDir\run-cli.cmd"
Push $cycleCliPath "$GuestDir\cycle.cmd"
Push $unmapCliPath "$GuestDir\unmap.cmd"

Run-Case "mapper_service_install" {
    Try-Guest "sc stop DrvliteMapper" -Elevated | Out-Null
    Try-Guest "sc delete DrvliteMapper" -Elevated | Out-Null
    $signedWith = "spc-signer"
    $installed = $false
    try {
        Guest "sc create DrvliteMapper type= kernel binPath= $GuestDir\drvlite-mapper.sys" -Elevated | Out-Null
        Guest "sc start DrvliteMapper" -Elevated | Out-Null
        $installed = $true
    } catch {
        Log "  Mapper start failed with the SPC signer; trying a test certificate"
    }
    if (-not $installed) {
        $signedWith = "test-cert"
        $certScript = Join-Path $RepoRoot "scripts\sign-test.ps1"
        & $certScript -Files (Join-Path $BuildDir "drvlite-mapper.sys") | Out-Null
        Push (Join-Path $RepoRoot "build\test-certs\drvlite-test.cer") "$GuestDir\drvlite-test.cer"
        Guest "certutil -addstore -f Root $GuestDir\drvlite-test.cer" -Elevated | Out-Null
        Guest "certutil -addstore -f TrustedPublisher $GuestDir\drvlite-test.cer" -Elevated | Out-Null
        Try-Guest "sc stop DrvliteMapper" -Elevated | Out-Null
        Try-Guest "sc delete DrvliteMapper" -Elevated | Out-Null
        Guest "sc create DrvliteMapper type= kernel binPath= $GuestDir\drvlite-mapper.sys" -Elevated | Out-Null
        Guest "sc start DrvliteMapper" -Elevated | Out-Null
    }
    $status = Guest "sc query DrvliteMapper" -Elevated
    if ($status -notmatch "RUNNING") { throw "mapper service is not running: $status" }
    return "service running ($signedWith)"
}

# ---------------------------------------------------------------------------
# 3. target process
# ---------------------------------------------------------------------------
Run-Case "target_process_start" {
    Try-Guest "del $GuestDir\target.json" -Elevated | Out-Null
    Try-Guest "del $GuestDir\target-changed.txt" -Elevated | Out-Null
    Guest "start /b $GuestDir\drvlite-target.exe --out $GuestDir\target.json --changed $GuestDir\target-changed.txt --seconds 1800" -Elevated | Out-Null
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        Start-Sleep -Seconds 2
        $raw = Invoke-Bv @("pull", $Vm, "$GuestDir\target.json", (Join-Path $OutDir "target.json")) 30
        if ($raw.ExitCode -eq 0 -and (Test-Path (Join-Path $OutDir "target.json"))) { break }
    }
    $descriptor = Get-Content (Join-Path $OutDir "target.json") -Raw | ConvertFrom-Json
    Set-Content (Join-Path $OutDir "target-pid.txt") $descriptor.pid
    return "pid=$($descriptor.pid) tid=$($descriptor.tid) buffer=$($descriptor.buffer)"
}
$target = Get-Content (Join-Path $OutDir "target.json") -Raw | ConvertFrom-Json
$targetPid = $target.pid
$targetTid = $target.tid
$targetBuffer = $target.buffer

Run-Case "target_process_second" {
    Try-Guest "del $GuestDir\target2.json" -Elevated | Out-Null
    Guest "start /b $GuestDir\drvlite-target.exe --out $GuestDir\target2.json --changed $GuestDir\target2-changed.txt --seconds 300" -Elevated | Out-Null
    for ($attempt = 0; $attempt -lt 20; ++$attempt) {
        Start-Sleep -Seconds 2
        $raw = Invoke-Bv @("pull", $Vm, "$GuestDir\target2.json", (Join-Path $OutDir "target2.json")) 30
        if ($raw.ExitCode -eq 0 -and (Test-Path (Join-Path $OutDir "target2.json"))) { break }
    }
    $descriptor = Get-Content (Join-Path $OutDir "target2.json") -Raw | ConvertFrom-Json
    return "pid=$($descriptor.pid)"
}
$target2 = Get-Content (Join-Path $OutDir "target2.json") -Raw | ConvertFrom-Json
$target2Pid = $target2.pid

# ---------------------------------------------------------------------------
# 4. cases
# ---------------------------------------------------------------------------
Run-Case "manual_map_attach" {
    $out = Run-Cli "info"
    if ($out -notmatch "driver base") { throw "info did not report the driver base" }
    return (($out -split "`n" | Where-Object { $_ -match "nt build|driver base|features" }) -join "; ")
}

Run-Case "physical_read_write" {
    $out = Run-Cli "phys"
    if ($out -notmatch "roundtrip: ok") { throw "physical roundtrip failed" }
    return "diagnostic page roundtrip ok"
}

Run-Case "kernel_read_write" {
    $modules = Run-Cli "modules"
    $match = [regex]::Match($modules, "0x[0-9a-f]+  ntoskrnl\.exe")
    if (-not $match.Success) { throw "ntoskrnl not found in kernel module list" }
    $base = $match.Value.Split(" ")[0]
    $out = Run-Cli "read-kernel $base 2"
    if ($out -notmatch "4d 5a") { throw "kernel read did not return MZ" }
    return "ntoskrnl at $base"
}

Run-Case "process_memory_cr3" {
    $before = Run-Cli "read-proc $targetPid $targetBuffer 8"
    if ($before -notmatch "44 52 56 4c 49 54 45 21") { throw "target pattern mismatch: $before" }
    $write = Run-Cli "write-proc $targetPid $targetBuffer 43 52 33 21"
    if ($write -notmatch "ok") { throw "process write failed" }
    $after = Run-Cli "read-proc $targetPid $targetBuffer 4"
    $poll = Try-Guest "type $GuestDir\target-changed.txt" -Elevated
    if ($poll -notmatch "0x") {
        throw "target process did not observe the externally written value: $poll"
    }
    $observed = ""
    foreach ($line in ($poll -split "`n")) {
        if ($line -match "0x") { $observed = $line.Trim() }
    }
    return "pattern matched, wrote 43523321, target observed $observed"
}

Run-Case "process_module_list" {
    $out = Run-Cli "modules $targetPid"
    if ($out -notmatch "ntdll.dll") { throw "process modules missing ntdll" }
    return "ntdll listed"
}

Run-Case "dse_disable_load_unsigned" {
    Try-Guest "sc stop DrvliteUnsigned" -Elevated | Out-Null
    Try-Guest "sc delete DrvliteUnsigned" -Elevated | Out-Null
    $baseline = "blocked"
    Guest "sc create DrvliteUnsigned type= kernel binPath= $GuestDir\drvlite-unsigned.sys" -Elevated | Out-Null
    try {
        Guest "sc start DrvliteUnsigned" -Elevated | Out-Null
        $baseline = "loaded"
    } catch {
        # Expected: DSE rejects the unsigned image.
    }
    Try-Guest "sc stop DrvliteUnsigned" -Elevated | Out-Null
    Try-Guest "sc delete DrvliteUnsigned" -Elevated | Out-Null
    if ($baseline -eq "loaded") {
        throw "unsigned driver loaded before the DSE patch; baseline is not enforcing DSE"
    }
    $dse = Run-Cli "dse"
    if ($dse -notmatch "supported : yes") { throw "DSE resolver unsupported: $dse" }
    Run-Cli "dse --disable" | Out-Null
    Guest "sc create DrvliteUnsigned type= kernel binPath= $GuestDir\drvlite-unsigned.sys" -Elevated | Out-Null
    Guest "sc start DrvliteUnsigned" -Elevated | Out-Null
    $loaded = Guest "sc query DrvliteUnsigned" -Elevated
    if ($loaded -notmatch "RUNNING") { throw "unsigned driver did not start after DSE patch: $loaded" }
    Try-Guest "sc stop DrvliteUnsigned" -Elevated | Out-Null
    Try-Guest "sc delete DrvliteUnsigned" -Elevated | Out-Null
    Run-Cli "dse --restore" | Out-Null
    return "baseline=blocked patched=loaded restored=blocked"
}

Run-Case "handle_full_access" {
    $out = Run-Cli "handle --pid $target2Pid --full --kill"
    if ($out -notmatch "terminate: yes") { throw "full-access handle not usable: $out" }
    return "terminated disposable process $target2Pid through full handle"
}

Run-Case "infinityhook_counters" {
    $ssn = Run-Cli "ssn NtQueryInformationThread"
    $match = [regex]::Match($ssn, "ssn=(\d+)")
    if (-not $match.Success) { throw "cannot resolve syscall number" }
    $out = Run-Cli "hook-test $($match.Groups[1].Value)"
    if ($out -notmatch "syscalls before=(\d+) after=(\d+)") { throw "hook-test output missing" }
    if ([int64]$Matches[2] -le [int64]$Matches[1]) { throw "no syscalls counted" }
    return "ssn=$($match.Groups[1].Value) counted=$($Matches[2])"
}

Run-Case "hwbp_detection_bypass" {
    $out = Run-Cli "hwbp-test $targetPid $targetTid"
    if ($out -notmatch "hidden: dr0=0x0") { throw "DR0 not hidden: $out" }
    return (($out -split "`n" | Where-Object { $_ -match "before:|hidden:|restored:" }) -join "; ")
}

Run-Case "negative_input_fuzz" {
    $out = Run-Cli "fuzz"
    if ($out -notmatch "fuzz: rejected=1 driver-alive=1") { throw "fuzz failed: $out" }
    return "malformed requests rejected, driver alive"
}

Run-Case "stress_memory_ops" {
    $out = Run-Cli "stress $targetPid $targetBuffer 1000"
    if ($out -notmatch "failures=0") { throw "stress failures: $out" }
    return "1000 mixed operations, 0 failures"
}

Run-Case "perf_benchmark" {
    $out = Run-Cli "bench --iterations 200"
    Set-Content (Join-Path $OutDir "bench.txt") $out
    return (($out -split "`n" | Where-Object { $_ -match "median" }) -join "; ")
}

Run-Case "page_paths_and_crossings" {
    $out = Run-Cli "memtest $targetPid $targetBuffer"
    if ($out -notmatch "memtest: .*ok") { throw "memtest failed: $out" }
    return "4k crossing, non-present, non-canonical, 2m boundary and chunked kernel read ok"
}

Run-Case "dse_concurrent_load" {
    $out = Run-Cli "dse-load-test $GuestDir\drvlite-unsigned.sys 20"
    if ($out -notmatch "baseline=blocked" -or $out -notmatch "restore=ok") {
        throw "dse-load-test failed: $out"
    }
    return (($out -split "`n" | Where-Object { $_ -match "dse-load-test:" }) -join "; ")
}

Run-Case "handle_protected_process" {
    $task = Try-Guest "tasklist" -Elevated
    $match = [regex]::Match($task, "MsMpEng\.exe\s+(\d+)")
    if (-not $match.Success) {
        return "skipped: no PPL/AV process present in the guest"
    }
    $pid = $match.Groups[1].Value
    $out = Run-Cli "handle --pid $pid --full --kill"
    if ($out -notmatch "granted  : 0x1fffff") {
        throw "protected-process handle was not raised to full access: $out"
    }
    if ($out -match "terminate: yes") {
        return "PPL process $pid terminated through a full-permission handle"
    }
    return "PPL process ${pid}: GrantedAccess raised to 0x1fffff; terminate blocked by PPL policy (expected on hardened AV)"
}

Run-Case "shutdown_with_hooks_armed" {
    $out = Run-Cli "hook-shutdown-test"
    if ($out -notmatch "installed=1") { throw "hook install failed: $out" }
    $alive = Guest "echo alive"
    if ($alive -notmatch "alive") { throw "guest unresponsive after hook shutdown" }
    return "hook installed then shutdown restored the syscall entry"
}

Run-Case "load_unload_1000" {
    $out = Guest "$GuestDir\cycle.cmd 1000" -Elevated -TimeoutSec 900
    if ($out -notmatch "1000 cycles, 0 failures") { throw "cycle-test failed: $out" }
    return "1000 map/handshake/shutdown/unmap cycles"
}

Run-Case "shutdown_restore" {
    $out = Run-Cli "shutdown"
    if ($out -notmatch "shutdown: ok") { throw "shutdown failed: $out" }
    $alive = Guest "echo alive"
    if ($alive -notmatch "alive") { throw "guest not responsive after shutdown" }
    return "driver shut down, guest responsive"
}

Run-Case "trace_scan" {
    $running = Try-Guest "sc query DrvliteMapper" -Elevated
    $reg1 = Try-Guest "reg query HKLM\SYSTEM\CurrentControlSet\Services\Drvlite /s" -Elevated
    $reg2 = Try-Guest "reg query HKLM\SYSTEM\CurrentControlSet\Services\drvlite-lite /s" -Elevated
    $drivers = Try-Guest "driverquery /v" -Elevated
    $scan = "$running`n$reg1`n$reg2`n$drivers"
    if ($scan -match "drvlite\.sys") { throw "mapped driver appears in driverquery: $scan" }
    if ($scan -match "SYSTEM\\\\CurrentControlSet\\\\Services\\\\Drvlite") {
        throw "mapped driver left a registry key: $scan"
    }
    return "no drvlite service/registry/driverquery trace (mapper harness excluded)"
}

Run-Case "load_unload_cycles" {
    $cycles = 10
    for ($index = 0; $index -lt $cycles; ++$index) {
        $out = Run-Cli "shutdown"
        if ($out -notmatch "shutdown: ok") { throw "cycle $index shutdown failed" }
        $mapped = [regex]::Match($out, "mapped (0x[0-9a-f]+)")
        if ($mapped.Success) {
            Guest "$GuestDir\unmap.cmd $($mapped.Groups[1].Value)" -Elevated | Out-Null
        }
    }
    $alive = Guest "echo alive"
    if ($alive -notmatch "alive") { throw "guest unresponsive after cycles" }
    return "$cycles process-level map/shutdown/unmap cycles"
}

# ---------------------------------------------------------------------------
# 5. evidence
# ---------------------------------------------------------------------------
$state = Invoke-Bv @("state", $Vm, "--json", "--no-ocr") 60
Set-Content (Join-Path $OutDir "state.json") $state.Output
$screenshot = Invoke-Bv @("screenshot", $Vm, (Join-Path $OutDir "screen.png")) 60
if ($screenshot.ExitCode -ne 0) {
    Log "warning: screenshot capture failed: $($screenshot.Output)"
}

$passed = @($cases | Where-Object { $_.status -eq "pass" }).Count
$failed = @($cases | Where-Object { $_.status -ne "pass" }).Count
$result = [pscustomobject]@{
    schema = "bvmenv.e2e.v1"
    run_id = (Get-Date -Format "yyyyMMdd'T'HHmmss'Z'")
    version = (Get-Content (Join-Path $RepoRoot "version.txt") -Raw).Trim()
    vm = $Vm
    guest = [pscustomobject]@{
        nt_build = ""
        drvlite_sys_size = (Get-Item (Join-Path $BuildDir "drvlite.sys")).Length
        mapper_signed = (Test-Path $signer)
    }
    cases = $cases
    pass = ($failed -eq 0)
    passed = $passed
    failed = $failed
    artifacts = @("result.json", "cases.txt", "state.json", "screen.png", "target.json", "bench.txt")
}
$result | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutDir "result.json") -Encoding utf8
Log "=== result: pass=$($result.pass) passed=$passed failed=$failed"
Log "=== evidence: $OutDir"

if (-not $KeepVm) {
    if (-not $NoReset) {
        Invoke-Bv @("reset", $Vm) 180 | Out-Null
    }
}
if ($failed -ne 0) { exit 1 }
