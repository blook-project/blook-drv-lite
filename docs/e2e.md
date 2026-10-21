# End-to-end tests (VM)

The e2e suite runs inside a disposable [bvmenv](https://github.com/blook-project/blook-vmware-environment)
guest. The host only drives `bvmenv` (every call has a hard timeout and is
killed on hang) and archives the evidence.

## Prerequisites

- `bvmenv` built from `blook-vmware-environment` (base image with test
  signing enabled).
- `blook-kchannel` checkout (`KCHANNEL_DIR` or `--kchannel_dir`).
- A test-signed **mapper**: this repo ships `tests/guest/mapper`
  (`drvlite-mapper.sys` + `drvlite-mapper-cli.exe`). It is a device-based test
  harness, not part of the production load model. A production run uses
  `blook-kdu` as the mapper instead.
- The test signer assets in `signer/` (inherited from `blook-drv`).

## Running

```powershell
./scripts/e2e.ps1 -Vm vm-drv-lite
# faster iteration, keep the VM:
./scripts/e2e.ps1 -Vm vm-drv-lite -SkipBuild -KeepVm
```

The script:
1. builds `-m release` (unless `-SkipBuild`), runs the host unit tests and
   signs `drvlite-mapper.sys`;
2. creates or resets the VM, starts it and polls a guest command until the
   guest answers (bounded: it never calls `wait-ready` blindly);
3. pushes `drvlite.sys`, `drvlite-cli.exe`, the mapper, the target process and
   the unsigned test driver to `C:\drvlite`;
4. installs and starts the mapper service;
5. starts `drvlite-target.exe` (one instance for memory tests, one disposable
   instance for the handle test);
6. runs the cases below, then captures `bvmenv state` JSON and a screenshot;
7. writes `docs/evidence/<yyyyMMdd>-drv-lite/` (result.json, cases.txt,
   state.json, screen.png, target.json, bench.txt) and resets the VM.

Any BSOD/hang is reported by `bvmenv state`/screenshot classification; the
VM is reset and the JSON records the failure.

## Cases

| Case | Pass criterion |
|---|---|
| mapper_service_install | mapper service starts (`sc query` RUNNING) |
| target_process_start | target publishes its JSON descriptor |
| manual_map_attach | `drvlite-cli run ... -- info` reports the mapped driver base and nt build |
| physical_read_write | diagnostic page read/write roundtrip verified by read-back |
| kernel_read_write | `modules` lists `ntoskrnl`, `read-kernel <base> 2` returns `4D 5A` |
| process_memory_cr3 | read of the target buffer returns the known `DRVLITE!` pattern, write changes it, the target observes the change |
| process_module_list | target module list contains `ntdll.dll` |
| dse_disable_load_unsigned | baseline unsigned load blocked, after `dse --disable` the unsigned driver loads, after restore it blocks again |
| handle_full_access | full-access handle opened with minimal request terminates a disposable process |
| infinityhook_counters | `hook-test` installs the trampoline, counts syscalls (> before), removes it |
| hwbp_detection_bypass | target thread DR0 is visible before, `0` while hiding is enabled, visible again after restore |
| negative_input_fuzz | malformed requests are rejected, driver still answers `query_info` |
| stress_memory_ops | 1000 mixed process read/write + kernel + physical operations, 0 failures |
| perf_benchmark | median/p99/throughput for physical/kernel/process read+write |
| shutdown_restore | `shutdown` returns ok and the guest stays responsive |
| trace_scan | no `drvlite` service/registry/`driverquery` trace from the mapped driver (the test mapper service is expected and excluded) |
| load_unload_cycles | 10 map → shutdown → unmap cycles without crash/hang |

## Result format

```json
{
  "run_id": "20261021T030000Z",
  "version": "1.0.0",
  "vm": "vm-drv-lite",
  "guest": { "drvlite_sys_size": 70656, "mapper_signed": true },
  "cases": [
    { "name": "physical_read_write", "status": "pass", "detail": "...", "duration_ms": 42 }
  ],
  "pass": true,
  "passed": 18,
  "failed": 0,
  "artifacts": ["result.json", "cases.txt", "state.json", "screen.png"]
}
```

`docs/evidence/` keeps the JSON, the case log, the state snapshot and a
screenshot. Raw driver/DBG logs are summarised (head/tail + hash) when large.
