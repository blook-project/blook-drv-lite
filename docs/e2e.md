# End-to-end tests (VM)

The e2e suite runs inside a disposable `bvmenv` guest (see
[blook-vmware-environment](https://github.com/blook-project/blook-vmware-environment)).
Nothing here runs on the build host; the host only drives `bvmenv` and
archives the evidence.

## Prerequisites

- `bvmenv` built (or on `PATH`), base image with test signing enabled.
- A manual-map loader: `blook-kdu` (`kdu map --driver ...`) or the kchannel
  reference mapper. The driver image is `build/windows/x64/release/drvlite.sys`.
- `drvlite-cli.exe` and the guest helpers `drvlite-target.exe` /
  `drvlite-unsigned.sys` built by the same xmake run.

## Running

```powershell
./scripts/e2e.ps1 -Vm vm-drv-lite
```

The script creates/forks the VM, pushes the test bundle, runs the cases
below, captures `result.json` plus logs and the host-side state JSON, resets
the VM, and writes everything to `docs/evidence/<YYYYMMDD>-drv-lite/`.

## Cases

| # | Case | Pass criterion |
|---|---|---|
| 1 | map + attach | `drvlite-cli map --image drvlite.sys` attaches; `info` reports the protocol version and features; `kc_state` is ACTIVE |
| 2 | physical read/write | read a known signature from a page, write a canary, read it back, restore the original byte; no faults |
| 3 | kernel virtual read | read `ntoskrnl` headers and `PsLoadedModuleList`; `modules` lists `ntoskrnl.exe`/`ci.dll` |
| 4 | process memory | `drvlite-target.exe` publishes a buffer; read it through the CR3 walk, write a new value, verify both via process and SDK; also verify a swapped-CR3 decoy is rejected (`process_query` validation) |
| 5 | module dump | dump a target module (e.g. `ntdll.dll` header page) and confirm `MZ`/`PE` and correct size/holes |
| 6 | DSE | baseline: `sc create/start drvlite-unsigned.sys` fails with error 577; `dse --disable`; load succeeds; `dse --restore`; load fails again |
| 7 | handle bypass | open a full-access handle to `drvlite-target.exe` (requested minimal access), `TerminateProcess`/`SuspendThread` through it succeeds, then close cleanly |
| 8 | InfinityHook | `hook install --filter <NtQueryInformationThread SSN>`; a user loop produces syscall counts; `hook stats` shows non-zero totals and filter hits |
| 9 | hwbp hide | target thread sets DR0 via `SetThreadContext`; a second process with the full-permission handle reads its context: values visible before, zeroed after `hook hide-dr --pid <target>` |
| 10 | shutdown/unload | `shutdown` returns; loader unmaps; device/registry/service scan (reg query, `driverquery`, object scan) shows no leftovers; the guest stays alive and responsive |
| 11 | stress | 200 attach/shutdown cycles and 10k mixed memory operations without BSOD/hang |
| 12 | negative input | bad pointers, non-canonical addresses, absurd lengths, oversized batches: all return errors, no bugcheck |

Any BSOD/hang is detected host-side by `bvmenv state` (stop code via screen
classification); the runner resets the VM and records the failure.

## Result format

```json
{
  "run_id": "20261021T030000Z",
  "version": "0.1.0",
  "guest": { "build": "26100", "test_signing": true },
  "cases": [
    { "name": "physical_read_write", "status": "pass", "detail": "...", "duration_ms": 42 }
  ],
  "artifacts": ["guest.log", "drvlite-cli.log", "state.json", "screen.png"],
  "pass": true
}
```

`docs/evidence/` keeps the JSON, the key CLI logs and a screenshot; large
raw logs are reduced to head/tail plus a sha256 and committed as a summary.
