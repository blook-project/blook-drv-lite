# Host-safety check: verifies that no blook-drv-lite kernel artefacts were
# installed on the physical host. Exit code 1 means a leftover was found.
#
#   ./scripts/check-host-clean.ps1
#
# Kernel experiments are only allowed inside bvmenv VMware guests; this script
# is the quick post-run verification that the host was not touched.
[CmdletBinding()]
param()
$ErrorActionPreference = "Stop"
$leftovers = @()

$serviceNames = @(
    "Drvlite", "drvlite", "DrvliteMapper", "DrvliteUnsigned",
    "drvlite-lite", "blook-drv-lite"
)
foreach ($name in $serviceNames) {
    $service = Get-Service -Name $name -ErrorAction SilentlyContinue
    if ($service) { $leftovers += "service: $name ($($service.Status))" }
    $driver = Get-CimInstance Win32_SystemDriver -Filter "Name='$name'" -ErrorAction SilentlyContinue
    if ($driver) { $leftovers += "driver: $name ($($driver.State))" }
}

foreach ($pattern in @("*drvlite*", "*blook*")) {
    Get-ChildItem "C:\Windows\System32\drivers" -Filter $pattern -ErrorAction SilentlyContinue |
        ForEach-Object { $leftovers += "driver file: $($_.FullName)" }
}

foreach ($name in $serviceNames) {
    $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$name"
    if (Test-Path $key) { $leftovers += "registry: $key" }
}

$testSigning = (bcdedit /enum "{current}" 2>$null | Select-String -Pattern "testsigning")
if ($testSigning -match "Yes") {
    $leftovers += "host boot configuration has testsigning=on"
}

if ($leftovers.Count -eq 0) {
    Write-Host "host clean: no drvlite services, driver files or registry keys"
    exit 0
}
Write-Host "HOST LEFTOVERS FOUND:" -ForegroundColor Red
$leftovers | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
exit 1
