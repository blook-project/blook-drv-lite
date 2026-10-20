# Architecture

`blook-drv-lite` is a manual-map-loadable Windows kernel driver plus a
user-mode SDK. It deliberately has **no hypervisor**, **no device object**,
**no registry footprint** and **no named objects**: the loader maps the image
and passes a single descriptor to the entry point, and all kernel/user
communication afterwards goes through
[blook-kchannel](https://github.com/blook-project/blook-kchannel).

```
┌────────────────────────── user mode ──────────────────────────┐
│ drvlite-cli / SDK                                             │
│   drvlite::session  ── kchannel user API ──┐                  │
│   manual map loader (blook-kdu or test mapper)                │
└────────────────────────────────────────────┼──────────────────┘
                                             │ anonymous shared section
┌────────────────────────── kernel ───────────┼──────────────────┐
│ drvlite.sys (mapped, no DriverObject)       │                  │
│   entry: kc_bootstrap -> kc_kernel_create   │                  │
│   ipc/dispatch: op >= 0x8000 -> handler ────┘                  │
│   mem/       physical, kernel VA, process VA (CR3 walk)        │
│   modules/   PsLoadedModuleList, PEB Ldr, dumps                │
│   dse/       ci.dll!g_CiOptions resolve/patch/restore          │
│   handle/    handle-table GrantedAccess rewrite                │
│   hook/      KiSystemCall64 entry trampoline, DR hiding        │
└────────────────────────────────────────────────────────────────┘
```

## Repository layout

| Path | Contents |
|---|---|
| `include/blook/drvlite/protocol.h` | application wire protocol (C ABI, shared by both sides) |
| `include/blook/drvlite/sdk.h` | public user-mode SDK API |
| `src/core` | portable, host-testable logic: page-table walk, range chunking, PE/pattern resolver, DSE resolver, handle-table math, x86 decoder, detour builder, protocol codec |
| `src/driver` | kernel driver (C++17): entry, kchannel adapter, dispatch, memory, modules, DSE, handles, hooks |
| `src/sdk` | user-mode SDK implementation (C++20) |
| `src/cli` | `drvlite-cli` |
| `tests/unit` | host tests (gtest) over `src/core` |
| `tests/guest` | disposable target process and unsigned driver used by e2e |
| `tests/e2e` | `bvmenv` driven end-to-end scripts |
| `scripts` | build, package, registry, e2e helpers |
| `signer` | SPC/TSA signing assets inherited from blook-drv |

The `src/core` split is the reason the host test suite exercises the real
decision logic (page-table math, resolver offsets, decoder prologues) instead
of a mock copy of it.

## Load model

1. `kc_attach()` creates an anonymous pagefile-backed section (no name) and
   generates the session key, salt and attach id.
2. The loader maps `drvlite.sys` into kernel memory. Its `DriverEntry`
   receives a **kernel copy** of `kc_bootstrap` in the `DriverObject`
   parameter slot; the image itself never registers with the I/O manager.
3. `kc_kernel_create()` maps the shared region (MDL + lock + cached system
   VA), writes the control page and starts the single PASSIVE_LEVEL worker.
4. `kc_attach_finish()` completes authentication; the session is `ACTIVE`.
5. Every SDK call is one kchannel `CALL` on an application opcode
   (>= 0x8000) carrying a `drvlite_req_header`.
6. `DRVLITE_OP_SHUTDOWN` restores every patch, stops the worker and signals
   the whole cleanup through the reply; the loader then unmaps the image.
   The driver never frees its own code.

## Memory subsystem

Everything is built on two injected primitives: a `phys_reader` (kernel:
`MmCopyMemory(MM_COPY_MEMORY_PHYSICAL)`) and a virtual reader/writer.

- **Physical read** – `MmCopyMemory` with `MM_COPY_MEMORY_PHYSICAL`, chunked
  at 4 KiB and 64 KiB.
- **Physical write** – the target page range is translated from kernel VA,
  mapped with `MmMapIoSpaceEx(PAGE_READWRITE | PAGE_NOCACHE)`, written, then
  unmapped. Cache attributes stay non-cached so the write reaches RAM, which
  is what makes writing read-only `.text` (DSE, hooks) possible without
  touching CR0.
- **Kernel virtual read/write** – the walker walks the current system CR3
  (`__readcr3()`) and reads/writes physical memory, so read-only and
  executable pages behave like any other page. Bad addresses return an error
  instead of faulting.
- **Process virtual read/write** – `PsLookupProcessByProcessId` →
  `KPROCESS.DirectoryTableBase` (EPROCESS+0x28) → validated by walking the
  PEB (`PEB.ImageBaseAddress`/`PEB.Ldr`) and comparing the image base with
  `PsGetProcessSectionBaseAddress`. Only then are translations trusted; the
  validation result is cached per request (not across requests, so a swapped
  CR3 is detected immediately). This is the `docs/reference/mref.cc` model:
  never attach, always read physical.
- **Ranges** – SDK requests are capped at `DRVLITE_MAX_TRANSFER` (0xFC00)
  and the driver chunks internally at page boundaries; the SDK chunks larger
  user requests and uses `DRVLITE_OP_MEM_BATCH_READ` for many small ranges.
- **Modules** – kernel modules from `PsLoadedModuleList`; target-process
  modules from `PEB.Ldr` through the CR3 walk; dumps use tolerant reads that
  zero-fill unmapped holes (module images always have them).

All lengths are checked against the request size before use, all addresses
are canonicality-checked, and page-table entry bits are validated, so a
malformed request returns `DRVLITE_ERR_PARAM` and can never fault the kernel.

## DSE override

`ci.dll` is located in the kernel module list, its live image is copied
through the kernel reader, and `src/core/dse_resolver.cpp`:

1. parses the PE export directory and finds `CiInitialize`;
2. follows its relative jump/call to the real `CipInitialize`;
3. scans that body for `mov dword ptr [rip+disp32], ecx|edx|imm32` and
   resolves the global inside a `.data` section.

The driver reads the current value, writes the requested value through the
physical write path, verifies the read-back and remembers the original value
for `DRVLITE_OP_DSE_RESTORE`. A legacy `g_CiEnabled` pattern scan is kept as
a fallback for Windows 7-era kernels. HVCI/VBS presence is checked before
patching and reported as unsupported instead of attempting a write that the
platform would reject.

## ObReference bypass

1. Resolve the target process (`PsLookupProcessByProcessId`) or thread.
2. Open a minimal handle **in the client process**: attach to the client,
   `ObOpenObjectByPointer(object, 0, NULL, minimal_access, *PsProcessType,
   UserMode, &handle)`.
3. Find `EPROCESS.ObjectTable`: scan candidate pointer fields of the client
   `EPROCESS` and accept the one whose `HANDLE_TABLE.TableCode` resolves the
   new handle to an entry whose 25-bit access field equals the requested
   access exactly (layout independent; probe of the table, not a hard-coded
   offset).
4. Rewrite that field to full access (`0x1FFFFF`) in place and verify.
5. The handle is returned to the SDK as a normal user handle; the SDK closes
   it with `CloseHandle`, so there is no kernel-side dangling reference.

The 25-bit GrantedAccess field position moved when Windows introduced handle
entry pointer obfuscation, so `src/core/handle_table.cpp` discovers the bit
offset from a known access value rather than hard-coding it, and patches the
same wide bitfield it discovered.

## InfinityHook

The syscall interception is a real `KiSystemCall64` entry hook, not an SSDT
patch:

1. `IA32_LSTAR` is read on every processor; all must agree on the syscall
   entry and the target must be inside the `ntoskrnl` image (otherwise the
   feature fails cleanly, e.g. on KVA-shadow layouts).
2. A trampoline is built by copying the entry instructions up to a safe
   splice point into executable pool memory, followed by an absolute jump to
   the assembly probe stub. The probe saves all volatile state (including
   XMM), derives the syscall number and trap-frame pointer, and chains the
   original entry.
3. A configurable syscall-number filter dispatches callbacks at PASSIVE
   level. The callback list drives:
   - **system-call statistics** (total/per-SSN counters),
   - **hardware-breakpoint detection bypass**: `NtGetContextThread` is
     resolved through the kernel service table (validated against exported
     service addresses) and detoured; queries for threads of the protected
     process return `Dr0-Dr3 = 0, Dr7 = 0`,
   - **syscall shield**: configured SSNs for a configured process return
     `STATUS_ACCESS_DENIED` without reaching the original handler.
4. `DRVLITE_OP_INFINITY_REMOVE` and shutdown walk the patch list in reverse,
   restoring the original bytes with the same IPI-synchronised patch
   primitive, then free the trampoline.

Code patching never races another processor: the physical page is mapped,
all processors are collected in an IPI barrier, the write happens while they
spin, and each processor executes a serialising instruction before release.
If the prologue cannot be decoded or relocated, installation fails without
touching the kernel.

## Unload and safety

`DRVLITE_OP_SHUTDOWN` runs on the worker and performs, in order: restore
InfinityHook, remove hooks, restore the DSE value, mark handles/stats
inactive, then stop the kchannel worker and destroy the transport. The reply
tells the SDK the cleanup stage succeeded; the SDK detaches and the loader
unmaps. Idle unload therefore leaves no thread, pool allocation, patch or
handle table entry behind.

## Threat model / non-goals

The driver is designed for research and self-testing on machines the operator
controls. It is not a persistence mechanism, does not hide from a hypervisor,
and its stealth is bounded by the mapped-image model (no device, no registry,
no module list entry). Anti-cheat or EDR products with kernel visibility may
still detect it; only use it where that is acceptable and legal.
