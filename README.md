# blook-drv-lite

A lite, **manual-map-loadable** Windows kernel driver plus a user-mode SDK.
No hypervisor, no device object, no registry key, no symbolic link: the
driver image is mapped by a loader (e.g. `blook-kdu`) and talks to user mode
through [blook-kchannel](https://github.com/blook-project/blook-kchannel).

> **Authorized security research only.** This project exists for kernel
> security research, reverse engineering and testing on machines you own or
> are explicitly authorized to test. Do not use it to violate any law or
> contract. See [LICENSE](LICENSE).

## Features

| Area | What it does |
|---|---|
| Memory | physical read (`MmCopyMemory`), safe physical write (`MmMapIoSpaceEx` with cache-attribute handling), kernel virtual read/write through the page tables (4 KiB/2 MiB/1 GiB), arbitrary-process read/write through a validated CR3 page-table walk, chunked/batched large transfers |
| Introspection | kernel module enumeration (`PsLoadedModuleList`), target-process module enumeration (PEB `Ldr` through the CR3 walk), module dumps with tolerant hole filling |
| Code integrity | resolve and patch `ci.dll!g_CiOptions` (fallback `g_CiEnabled` scan), verify the write, expose status, restore the original value; feature-detected and clean-failing on unsupported builds/HVCI |
| Handles | locate the target `EPROCESS`, open a minimal handle in the client process, rewrite the handle-table entry `GrantedAccess` to full access, close/restore cleanly |
| Syscall interception | InfinityHook-style `KiSystemCall64` entry trampoline with callback dispatch on a syscall-number filter (per-process filters, counters), used to implement a hardware-breakpoint detection bypass (sanitizes debug-register state returned by `NtGetContextThread`) and a syscall filter/shield |
| Stealth | no `IoCreateDevice`, no registry, no named objects, manual-map entry parameter carries the kchannel bootstrap, graceful in-place shutdown that restores every patch before the loader unmaps the image |

## Load model

```
user process                          kernel
-----------                          ------
kc_attach()  ── anonymous section ──► kc_kernel_create()  (entry)
kc_get_bootstrap()
manual map loader ──── image + bootstrap ───► DriverEntry(bootstrap)
kc_attach_finish() ◄── handshake ──────────── kc_kernel_start()
kc_call(op, ...) ──── shared-memory rings ──► handler dispatch
kc_call(SHUTDOWN) ──────────────────────────► restore patches, stop worker
loader unmaps driver image
```

The driver is loaded manually: `DriverEntry` receives a pointer to a
**kernel copy** of `kc_bootstrap` in its first parameter slot. There is no
`DriverUnload`, no device object and no service entry; unload is the
loader's unmap after the `DRVLITE_OP_SHUTDOWN` handler has restored every
patch and drained the worker.

## Build

Requirements: Windows 10/11 x64, VS 2022/2026 with the WDK and SDK
(10.0.26100.0 tested), [xmake](https://xmake.io) 3.x, and a checkout of
`blook-kchannel` (headers are required; sources are compiled in).

```powershell
xmake f -y -m release --kchannel_dir=D:\blook-kernel-project\blook-kchannel
xmake -y
xmake run drvlite-tests        # host unit tests
```

Without `--kchannel_dir` the build looks for `..\blook-kchannel` next to the
repository.

Documentation:
[architecture](docs/architecture.md) ·
[protocol / 22 opcodes](docs/protocol.md) ·
[SDK guide](docs/sdk.md) ·
[safety and limits](docs/safety.md) ·
[performance](docs/performance.md) ·
[VM e2e](docs/e2e.md)

## CLI

```text
drvlite-cli run --image drvlite.sys \
    --map-cmd "kdu map --driver {image} --bootstrap {bootstrap}" \
    -- <command>

commands:
  info                          build/ABI/features/bases
  phys                          physical read/write roundtrip (diagnostic page)
  read-phys/write-phys <a> ...  raw physical access
  read-kernel/write-kernel <a>  kernel virtual access
  read-proc/write-proc <pid>    arbitrary-process memory (CR3 walk)
  process <pid>                 CR3/PEB/image base + validation
  modules [pid]                 kernel or process module list
  dump <pid> <a> <n> <file>     module dump
  dse [--disable|--restore]     code-integrity override
  handle --pid N [--full] [--kill]
  thread-dr <pid> <tid>         read thread debug registers
  hook-test [ssn...]            install/count/remove the syscall trampoline
  hwbp-test <pid> <tid>         DR cloak before/hidden/restored
  ssn <ntdll-export>            resolve a syscall number
  stress <pid> <addr> <n>       1000x mixed memory operations
  bench [--iterations N]        latency/throughput per mode
  fuzz                          malformed-request safety checks
  shutdown                      restore patches, stop the worker
```

`drvlite-cli` and the SDK are documented in [docs/sdk.md](docs/sdk.md); the
wire protocol is in [docs/protocol.md](docs/protocol.md). A complete example
program lives in `examples/read_process_dse.cpp` (built as `drvlite-example`).

## Signing

Test images are signed with the `signer/` assets inherited from the
`blook-drv` project (SPC + local TSA); CI uses an ephemeral test certificate
only. No private key is stored in this repository. Manual mapping does not
require a valid signature, but the guest test helper drivers used by the e2e
suite are test-signed.

## Supported builds

| Windows | Driver | DSE override | InfinityHook / DR-hide |
|---|---|---|---|
| Windows 10 19041 / 19045 x64 | yes | yes | layout-checked, clean failure otherwise |
| Windows 11 22000 / 22621 / 22631 x64 | yes | yes | layout-checked, clean failure otherwise |
| Windows 11 26100 x64 | yes | yes | validated in the e2e VM |
| KVA shadow (`KiSystemCall64Shadow`) | yes | yes | `DRVLITE_ERR_UNSUPPORTED` (no shadow mapping for the trampoline) |
| HVCI / VBS enabled | memory r/w where the OS allows | rejected/reported unsupported | unsupported |
| ARM64 | not supported | — | — |

Capabilities are reported per session through `query_info().features`; every
unsupported operation returns a status code instead of crashing. See
[docs/safety.md](docs/safety.md) for the full risk matrix.

## Host safety (hard rule)

**Kernel experiments only run inside a bvmenv VMware guest fork.** Loading a
driver, manual mapping, DSE patching, InfinityHook, physical/CR3 access and
payload mapping must never happen on the physical host. This is enforced, not
just documented:

- `drvlite-cli run` and `drvlite-mapper-cli` refuse to start unless
  `DRVLITE_ALLOW_KERNEL_LIVE=1` is set; `scripts/e2e.ps1` sets it only inside
  its `bvmenv exec` guest commands.
- `scripts/e2e.ps1` requires an explicit `-Vm <name>`, rejects host-like
  names, and creates/resets that VM through `bvmenv` only (no `vmrun`, no
  host fallback).
- `scripts/check-host-clean.ps1` verifies the host has no `drvlite*`/`blook*`
  services, driver files, registry keys or test-signing changes.

## Safety

All user input crosses the kchannel transport and is length/range validated
before it is used; invalid parameters return a status, they never fault the
kernel. Anything the driver changes in the kernel (DSE flag, handle access,
syscall trampoline) is recorded and restored on shutdown; the e2e suite
exercises this including a 10x map/shutdown/unmap cycle. See
[docs/safety.md](docs/safety.md) for the honest limits of each feature.

## License

MIT. See [LICENSE](LICENSE).
