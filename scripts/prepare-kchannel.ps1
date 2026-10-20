# Fetches/updates the pinned blook-kchannel checkout used by the build.
#
#   ./scripts/prepare-kchannel.ps1                       # ../blook-kchannel
#   ./scripts/prepare-kchannel.ps1 -Path D:\kchannel
#   ./scripts/prepare-kchannel.ps1 -Commit <sha>
#
# CI clones the same repository at the pinned commit; keep this in sync with
# .github/workflows/ci.yml (KCHANNEL_COMMIT).
[CmdletBinding()]
param(
    [string]$Path = (Join-Path (Split-Path -Parent $PSScriptRoot) "..\blook-kchannel"),
    [string]$Repo = "https://github.com/blook-project/blook-kchannel",
    [string]$Commit = "8c2a056065993e0eae85c5b6d11cd0c8472c671e"
)
$ErrorActionPreference = "Stop"
$Path = [IO.Path]::GetFullPath($Path)
if (-not (Test-Path (Join-Path $Path "include\blook\kchannel.h"))) {
    if (Test-Path $Path) { Remove-Item -Recurse -Force $Path }
    git clone --quiet $Repo $Path
}
git -C $Path fetch --quiet origin
git -C $Path checkout --quiet $Commit
$actual = (git -C $Path rev-parse HEAD).Trim()
if ($actual -ne $Commit) { throw "kchannel checkout is $actual, expected $Commit" }
Write-Host "kchannel ready at $Path ($actual)"
