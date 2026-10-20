# Local build pipeline: configure -> build -> host tests -> sign.
#
#   ./scripts/Build.ps1                                   # build + test + sign
#   ./scripts/Build.ps1 -NoTests -NoSign                  # fast bring-up
#   ./scripts/Build.ps1 -KchannelDir D:\blook-kchannel    # explicit checkout
#   ./scripts/Build.ps1 -WdkDir 'C:\Program Files (x86)\Windows Kits\10'
#
# The kernel image is signed with the in-repo signer assets inherited from
# blook-drv (SPC + local TSA). CI only test-signs; this script is what a
# release run uses.
[CmdletBinding()]
param(
    [switch]$NoTests,
    [switch]$NoSign,
    [string]$Mode = "releasedbg",
    [string]$BuildDir = "build",
    [string]$KchannelDir = "",
    [string]$WdkDir = ""
)
$ErrorActionPreference = "Continue"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildRoot = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
$BinDir = Join-Path $BuildRoot "windows\x64\$Mode"
$Driver = Join-Path $BinDir "drvlite.sys"
$xmake = (Get-Command xmake -ErrorAction SilentlyContinue).Source
if (-not $xmake) {
    foreach ($candidate in @("$env:USERPROFILE\xmake\xmake.exe", "$env:LOCALAPPDATA\xmake\xmake.exe")) {
        if (Test-Path $candidate) { $xmake = $candidate; break }
    }
}
if (-not $xmake) { throw "xmake not found" }

function Invoke-Step([string]$label, [scriptblock]$body) {
    Write-Host "=== $label"
    & $body
    if ($LASTEXITCODE -ne 0) { throw "$label failed ($LASTEXITCODE)" }
}

Push-Location $RepoRoot
try {
    $configure = @("f", "-y", "-m", $Mode)
    if ($KchannelDir) { $configure += "--kchannel_dir=$KchannelDir" }
    if ($WdkDir) { $configure += "--wdk_dir=$WdkDir" }
    Invoke-Step "configure" { & $xmake @configure }

    Invoke-Step "build" { & $xmake build -a }

    if (-not $NoTests) {
        Invoke-Step "host unit tests" { & $xmake run drvlite-tests }
    }

    if (-not $NoSign) {
        if (-not (Test-Path $Driver)) { throw "no driver image at $Driver" }
        $signer = Join-Path $RepoRoot "signer\spcsign\spcsign.exe"
        if (-not (Test-Path $signer)) { throw "missing signer: $signer" }
        $signArgs = @("sign", "--sha1", "--cert", "XINDA",
            "--tsa-time", "2013-01-01T00:00:00Z",
            "--chain", (Join-Path $RepoRoot "signer\certs\verisign-cscs-2010.cer"),
            "--chain", (Join-Path $RepoRoot "signer\certs\verisign-g5-mcvr.cer"),
            $Driver)
        Invoke-Step "sign driver" { & $signer @signArgs }
        Get-FileHash -Algorithm SHA256 $Driver |
            ForEach-Object { Write-Host "sha256 $($_.Hash.ToLowerInvariant())" }
    }
    Write-Host "=== done"
} finally {
    Pop-Location
}
