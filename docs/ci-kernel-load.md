# CI kernel-load smoke (real SCM execution, no test signing)

`kernel-load-smoke` is a required job in `.github/workflows/ci.yml`. It runs on
a stock `windows-latest` runner and proves that the built driver actually
executes in kernel mode, using the repository's **real SPC signing scheme** —
no test-signing mode and no boot configuration changes.

## Recipe

The implementation is `scripts/ci-kernel-smoke.ps1`; `kchannel`, `kdu` and
`drv` can copy it directly. The job needs the `SPC_PFX_BASE64` and
`SPC_PFX_PASSWORD` repository secrets (base64 PFX + its password).

1. **Import the signing certificate.** The shipped signer's `--pfx` argument
   is broken, so import the PFX into `Cert:\CurrentUser\My` and sign by
   certificate subject:

   ```powershell
   [IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($env:SPC_PFX_BASE64))
   $secure = ConvertTo-SecureString $env:SPC_PFX_PASSWORD -AsPlainText -Force
   Import-PfxCertificate -FilePath $pfxPath -CertStoreLocation Cert:\CurrentUser\My -Password $secure
   ```

2. **Trust the local TSA root and the legacy cross-signing chain** (admin;
   `certutil` targets `LocalMachine` in CI):

   ```powershell
   certutil.exe -addstore -f root  signer\tsa\tsa-root.cer
   certutil.exe -addstore -f CA    signer\certs\verisign-cscs-2010.cer
   certutil.exe -addstore -f CA    signer\certs\verisign-g5-mcvr.cer
   ```

   The TSA root is required for the `2013-01-01T00:00:00Z` countersignature to
   validate; without it `signtool verify /kp` fails `0x80096005` and the load
   fails `577` for the wrong reason.

3. **Sign with SHA-1, not SHA-256** (the current signer requires it):

   ```powershell
   spcsign.exe sign --sha1 --cert XINDA --tsa-time 2013-01-01T00:00:00Z `
       --chain signer\certs\verisign-cscs-2010.cer `
       --chain signer\certs\verisign-g5-mcvr.cer `
       build\windows\x64\release\drvlite.sys
   ```

4. **Trust the signature's own certificates** — export them from the signed
   image and add them to `Root` and `TrustedPublisher` (this includes the
   XINDA leaf and the local TSA chain):

   ```powershell
   spcsign.exe export build\windows\x64\release\drvlite.sys --out $certDir
   foreach ($cer in Get-ChildItem $certDir -Filter *.cer) {
       certutil.exe -addstore -f Root $cer.FullName
       certutil.exe -addstore -f TrustedPublisher $cer.FullName
   }
   ```

5. **Assert the kernel-mode policy before loading**:

   ```powershell
   signtool.exe verify /kp /v build\windows\x64\release\drvlite.sys
   # must print: Successfully verified ... Number of warnings: 0 ... Number of errors: 0
   ```

6. **Load and drive it** (the CLI writes the kchannel bootstrap into the
   service's `Parameters\Bootstrap` registry value, then starts the service
   and handshakes):

   ```powershell
   sc.exe create Drvlite type= kernel binPath= <driver path>
   drvlite-cli.exe run --allow-kernel-live --mode copy --service Drvlite `
       --image <driver> --bootstrap $env:RUNNER_TEMP\boot.bin `
       --map-cmd <start-drv.cmd> -- info
   sc.exe query Drvlite          # must show STATE 4 RUNNING
   drvlite-cli.exe run ... -- read-kernel <kernel base> 2   # must print 4d 5a
   ```

   Finally `sc stop` + `sc delete` in a `finally` block.

## Known signer issue: SHA-256 page hashes

`spcsign sign` **without** `--sha1` (SHA-256 digest) produces a signature that
`Get-AuthenticodeSignature` reports as `HashMismatch`: the signed page-hash
attributes do not match the image. On 26100 this is rejected by kernel code
integrity (`0x80096010` / `577`), which looks like a trust problem but is a
signer bug. Use `--sha1` (valid Authenticode) until the signer's SHA-256
page-hash path is fixed.

## Why this is trustworthy

- It runs on a stock GitHub-hosted runner with Secure Boot/code-integrity
  defaults — no `testsigning`, no `bcdedit`, no custom boot configuration.
- `/kp` verification is asserted before the load, so a failure cannot be
  mistaken for a policy/trust issue.
- The client is a separate user-mode process connecting over the manual-map
  ABI: `info` proves DriverEntry and the kchannel handshake ran; `read-kernel`
  proves an application opcode executed with the CR3 page walk.
