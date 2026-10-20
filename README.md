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
repository. See [docs/architecture.md](docs/architecture.md) for the module
breakdown and [docs/e2e.md](docs/e2e.md) for the VM end-to-end procedure.

## CLI

```text
drvlite-cli load   --image blook-drv-lite.sys   # map + attach (needs kdu/mapper)
drvlite-cli info
drvlite-cli read-phys  0x1000 16
drvlite-cli read-va    0xfffff80000000000 16
drvlite-cli read-proc  <pid> 0x7ff... 32
drvlite-cli write-proc <pid> 0x... 2 dad0
drvlite-cli modules [--pid N]
drvlite-cli dse [--disable|--restore]
drvlite-cli handle --pid N --access full
drvlite-cli hook install|remove|stats|hide-dr --pid N
drvlite-cli shutdown
```

`drvlite-cli` is documented further in [docs/architecture.md](docs/architecture.md).

## Signing

Test images are signed with the `signer/` assets inherited from the
`blook-drv` project (SPC + local TSA); CI uses an ephemeral test certificate
only. No private key is stored in this repository. Manual mapping does not
require a valid signature, but the guest test helper drivers used by the e2e
suite are test-signed.

## Supported builds

| Windows | Status |
|---|---|
| Windows 10 19041/19045 x64 | supported (validated when a test VM is available) |
| Windows 11 22000/22621/22631/26100 x64 | supported, e2e validated on 26100 |
| ARM64 | not supported |
| HVCI / VBS enabled | DSE patch and code-patching features refuse to run with a clean `DRVLITE_ERR_UNSUPPORTED`; memory read/write still works where the OS allows it |

## Safety

All user input crosses the kchannel transport and is length/range validated
before it is used; invalid parameters return a status, they never fault the
kernel. Code patching is atomic with respect to other processors through an
IPI barrier and is always undone by `shutdown`. Anything the driver changes
in the kernel (DSE flag, handle access, syscall trampoline, function hooks)
is recorded and restored on shutdown.

## License

MIT. See [LICENSE](LICENSE).
