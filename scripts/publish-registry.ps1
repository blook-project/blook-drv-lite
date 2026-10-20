# Publishes this release into blook-project/blook-xmake-repo.
#
#   ./scripts/publish-registry.ps1 -Version 0.1.0 -DryRun         # local check
#   ./scripts/publish-registry.ps1 -Version 0.1.0                # clone + push
#   ./scripts/publish-registry.ps1 -Version 0.1.0 -ManifestFile .\xmake-package\xmake.lua
#
# The registry owns the real logic (scripts/publish-registry.ps1 there); this
# wrapper locates a registry checkout (or the one used by the team at
# D:\blook-kernel-project\.deps\blook-xmake-repo) and forwards to it. The
# package manifest already exists in the registry, so -ManifestFile is only
# needed for the first publish of a new package.
[CmdletBinding()]
param(
    [string]$Package = "blook-drv-lite",
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$ManifestFile = "",
    [string]$RegistryDir = "",
    [switch]$DryRun,
    [switch]$NoPush,
    [string]$Sha256 = "",
    [string]$OutDir = "artifacts"
)
$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

if (-not $RegistryDir) {
    $candidates = @(
        (Join-Path $RepoRoot "..\..\.deps\blook-xmake-repo"),
        (Join-Path $env:TEMP "blook-xmake-repo")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate "scripts\publish-registry.ps1")) {
            $RegistryDir = [IO.Path]::GetFullPath($candidate)
            break
        }
    }
}
if (-not $RegistryDir) {
    $RegistryDir = Join-Path $env:TEMP ("blook-xmake-repo-" + [guid]::NewGuid())
    gh repo clone blook-project/blook-xmake-repo $RegistryDir -- --depth 1
}
$publisher = Join-Path $RegistryDir "scripts\publish-registry.ps1"
if (-not (Test-Path $publisher)) { throw "registry publisher not found: $publisher" }

if (-not $Sha256) {
    $shaFile = Get-ChildItem $OutDir -Filter "blook-drv-lite-$Version-windows-x64.zip.sha256" -ErrorAction SilentlyContinue
    if ($shaFile) {
        $Sha256 = (Get-Content $shaFile.FullName -Raw).Split(" ")[0]
    }
}
if (-not $ManifestFile) {
    $registryManifest = Join-Path $RegistryDir "packages\b\$Package\xmake.lua"
    $repoManifest = Join-Path $RepoRoot "xmake-package\xmake.lua"
    if (-not (Test-Path $registryManifest) -and (Test-Path $repoManifest)) {
        $ManifestFile = $repoManifest
    }
}

$arguments = @{
    Package = $Package
    Version = $Version
    RegistryDir = $RegistryDir
}
if ($Sha256) { $arguments.Sha256 = $Sha256 }
if ($ManifestFile) { $arguments.ManifestFile = $ManifestFile }
if ($DryRun) { $arguments.DryRun = $true }
if ($NoPush) { $arguments.NoPush = $true }

Write-Host "registry: $RegistryDir"
& $publisher @arguments
exit $LASTEXITCODE
