# CLI reference and JSON schema

`drvlite-cli` is the reference front end for the SDK. Every kernel operation
goes through a `run` wrapper that performs the two-phase load (kchannel
session → external mapper → handshake), so all commands print the driver's
mapped base and then execute one subcommand in the same process.

**Host safety:** `run` and `cycle-test` refuse to start unless
`DRVLITE_ALLOW_KERNEL_LIVE=1` is set. `scripts/e2e.ps1` sets it only inside
`bvmenv exec` guest commands; there is no host execution path.

## Commands

| Command | Purpose |
|---|---|
| `run --image <sys> --map-cmd <template> [--region N] -- <cmd>` | attach, map, handshake, run one command |
| `cycle-test --image <sys> --cycles N` | fast in-process map/handshake/shutdown/unmap loop |
| `info` | build, ABI, feature bitmap, driver/kernel bases |
| `phys` | diagnostic-page physical read/write roundtrip |
| `read-phys <addr> <size>` / `write-phys <addr> <hex>` | raw physical memory |
| `read-kernel <addr> <size>` / `write-kernel <addr> <hex>` | kernel virtual memory |
| `read-proc <pid> <addr> <size>` / `write-proc <pid> <addr> <hex>` | process memory (CR3 walk) |
| `process <pid>` | CR3/PEB/image base + validation state |
| `modules [pid]` | kernel (pid 0) or process module list |
| `dump <pid> <addr> <size> <file>` | module dump (holes zero-filled) |
| `dse [--disable|--restore]` | code-integrity override |
| `handle --pid N [--full] [--kill]` | handle bypass, optional destructive proof |
| `thread-dr <pid> <tid>` | read thread debug registers through a full handle |
| `hook-test [ssn...]` | install trampoline, count syscalls, remove |
| `hook-shutdown-test` | install the hook then shut down (restore path) |
| `hwbp-test <pid> <tid>` | DR cloak before/hidden/restored |
| `ssn <ntdll-export>` | resolve a syscall number from the local ntdll stub |
| `stress <pid> <addr> <n>` | mixed memory operations, failure count |
| `memtest <pid> <buffer>` | page crossing, non-present, non-canonical, 2 MiB boundary, chunked read |
| `dse-load-test <unsigned.sys> [n]` | DSE patch with a concurrent loader thread |
| `bench [--iterations N]` | latency/throughput per mode |
| `fuzz` | malformed-request rejection checks |
| `shutdown` | restore every patch, stop the worker |
| `version` | CLI + protocol version |

## `--json`

`info`, `modules`, and `dse` accept `--json`; the other commands print
human-readable `key: value` / `key=value` lines that are easy to parse.

`info --json`:

```json
{
  "nt_build": 26100,
  "abi": "1.0",
  "features": 511,
  "driver_base": "0xfffff80012340000",
  "driver_size": 70656,
  "kernel_base": "0xfffff80200000000",
  "kernel_size": 12582912,
  "client_pid": 4321,
  "client_eprocess": "0xffffa10f12345680"
}
```

`modules [pid] --json`:

```json
[
  { "base": "0xfffff80200000000", "size": 12582912, "name": "ntoskrnl.exe" },
  { "base": "0xfffff80210000000", "size": 1204224, "name": "ci.dll" }
]
```

`dse --json`:

```json
{
  "supported": true,
  "patched": false,
  "address": "0xfffff80218a40108",
  "original": 6,
  "current": 6,
  "ci_base": "0xfffff80210000000",
  "ci_size": 1204224,
  "via": 1,
  "error": 0
}
```

`bench` output line format:

```
phys-read-4k             median=   18.43 us  p99=   24.10 us   212.1 MiB/s  failures=0
```

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | operation or check failed |
| 2 | usage error / no session |
| 3 | refused: host kernel execution not allowed (no `DRVLITE_ALLOW_KERNEL_LIVE=1`) |
