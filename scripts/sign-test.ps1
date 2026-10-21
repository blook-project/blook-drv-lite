# Creates (once) an ephemeral code-signing certificate and signs driver
# binaries with it. This is the fallback path for the e2e VM when the
# blook-drv SPC signer is not accepted by the guest's test-signing policy.
#
#   ./scripts/sign-test.ps1 -Files build/windows/x64/release/drvlite-mapper.sys
#   ./scripts/sign-test.ps1 -EnsureOnly
#
# The certificate lives in build/test-certs/ (gitignored) and is imported into
# the guest's Root/TrustedPublisher stores by scripts/e2e.ps1.
[CmdletBinding()]
param(
    [string[]]$Files = @(),
    [string]$OutDir = "build\test-certs",
    [switch]$EnsureOnly
)
$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutDir = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $RepoRoot $OutDir }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$pfxPath = Join-Path $OutDir "drvlite-test.pfx"
$cerPath = Join-Path $OutDir "drvlite-test.cer"
$passwordText = "drvlite"
$password = ConvertTo-SecureString -String $passwordText -AsPlainText -Force

function Ensure-Certificate() {
    if ((Test-Path $pfxPath) -and (Test-Path $cerPath)) { return }
    $cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq "CN=drvlite-test" } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert `
            -Subject "CN=drvlite-test" -KeyExportPolicy Exportable `
            -KeyUsage DigitalSignature -KeyLength 2048 -HashAlgorithm sha256 `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -NotAfter (Get-Date).AddYears(3)
        $cert = Get-Item ("Cert:\CurrentUser\My\" + $cert.Thumbprint)
        $cert.FriendlyName = "drvlite e2e test signing"
    }
    Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $password | Out-Null
    Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null
    Write-Host "test certificate: $($cert.Thumbprint) -> $OutDir"
}

function Find-SignTool() {
    $candidates = @(
        "Y:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe",
        "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    $command = Get-Command signtool -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw "signtool not found"
}

Ensure-Certificate
if ($EnsureOnly) {
    Write-Host "certificate ready: $cerPath"
    return
}
if ($Files.Count -eq 0) { throw "no files to sign" }
$signTool = Find-SignTool
foreach ($file in $Files) {
    if (-not (Test-Path $file)) { throw "missing file: $file" }
    Write-Host "signing $file"
    & $signTool sign /f $pfxPath /p $passwordText /fd sha256 /a $file
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $file" }
    & $signTool verify /pa $file | Out-Null
}
Write-Host "signed $($Files.Count) file(s) with the ephemeral test certificate"
