# Builds the distributable archive and its sha256.
#
#   ./scripts/package.ps1 -Mode release -OutDir artifacts
#
# Asset contract (see blook-xmake-repo docs/RELEASE.md):
#   blook-drv-lite-<version>-windows-x64.zip
#   blook-drv-lite-<version>-windows-x64.zip.sha256
# Layout: include/ lib/x64/ bin/ scripts/ docs/ + top-level docs.
[CmdletBinding()]
param(
    [ValidateSet("dev", "release")]
    [string]$Mode = "release",
    [string]$OutDir = "artifacts",
    [string]$BuildDir = "build"
)
$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Version = (Get-Content (Join-Path $RepoRoot "version.txt") -Raw).Trim()
$BuildRoot = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
$ModeDir = if ($Mode -eq "release") { "release" } else { "releasedbg" }
$BinDir = Join-Path $BuildRoot "windows\x64\$ModeDir"
$Out = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $RepoRoot $OutDir }

New-Item -ItemType Directory -Force $Out | Out-Null
$stage = Join-Path $Out "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$stageBin = Join-Path $stage "bin\x64"
$stageLib = Join-Path $stage "lib\x64"
New-Item -ItemType Directory -Force $stageBin, $stageLib | Out-Null

$required = @("drvlite.sys", "drvlite-cli.exe")
foreach ($name in $required) {
    $path = Join-Path $BinDir $name
    if (-not (Test-Path $path)) { throw "missing build artifact: $path" }
    Copy-Item $path $stageBin
}
foreach ($optional in @("drvlite-target.exe", "drvlite-unsigned.sys")) {
    $path = Join-Path $BinDir $optional
    if (Test-Path $path) { Copy-Item $path $stageBin }
}
$sdkLib = Join-Path $BinDir "drvlite-sdk.lib"
if (Test-Path $sdkLib) {
    Copy-Item $sdkLib $stageLib
} elseif ($Mode -eq "release") {
    throw "missing SDK library: $sdkLib"
}
foreach ($item in @("include", "scripts", "docs")) {
    $source = Join-Path $RepoRoot $item
    if (Test-Path $source) {
        Copy-Item $source (Join-Path $stage $item) -Recurse
    }
}
$share = Join-Path $stage "share"
New-Item -ItemType Directory -Force $share | Out-Null
$xmakePackage = Join-Path $RepoRoot "xmake-package"
if (Test-Path $xmakePackage) {
    Copy-Item $xmakePackage (Join-Path $share "xmake-package") -Recurse
}
foreach ($file in @("README.md", "LICENSE", "CHANGELOG.md", "version.txt")) {
    $source = Join-Path $RepoRoot $file
    if (Test-Path $source) { Copy-Item $source $stage }
}

$zipName = "blook-drv-lite-$Version-windows-x64.zip"
$zipPath = Join-Path $Out $zipName
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipPath -CompressionLevel Optimal
Remove-Item -Recurse -Force $stage

$hash = (Get-FileHash -Algorithm SHA256 $zipPath).Hash.ToLowerInvariant()
$hashLine = "$hash  $zipName"
Set-Content -Path (Join-Path $Out "$zipName.sha256") -Value $hashLine -Encoding ascii
Write-Host "package: $zipPath"
Write-Host "sha256 : $hash"
