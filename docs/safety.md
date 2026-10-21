# Safety, limits and honest risk notes

This project is a research tool. It deliberately performs operations that
break kernel security invariants; run it only on machines you own or are
explicitly authorized to test, in a disposable VM. See `LICENSE`.

## What the driver changes in the kernel

| Change | How it is made safe | What can still go wrong |
|---|---|---|
| DSE flag (`ci.dll!g_CiOptions`) | address resolved from the live image (PE export → `CipInitialize` → RIP-relative store), validated to lie in a `.data` section, write verified by read-back, original remembered and restored on shutdown | a wrong resolution could corrupt a global; HVCI/VBS may reject or ignore the write; the flag is a global security bypass for the whole machine while dirty |
| Handle table `GrantedAccess` | a fresh minimal handle is opened first, the 25-bit field is found by matching the observed access value, patched and read back; the handle is returned to the SDK and closed by it | the client process can use rights its token never had; a concurrent handle-table entry update is not locked (16-byte entry), so this is inherently racy on a busy process |
| Syscall entry | no `ntoskrnl` code is patched: a trampoline in executable pool memory is installed through the per-CPU `IA32_LSTAR` MSR, and every original value is saved per CPU and restored | unsupported on layouts that differ from the documented prologue (KVA shadow). The trampoline memory must be executable; if pool-exec is denied the feature fails cleanly. A 20 ms grace period is used before freeing the trampoline on uninstall |
| NtGetContextThread cloak | the original service is called from the hook and only the CONTEXT debug-register fields returned to user mode are zeroed, inside a `__try/__except` | a query by the protected process itself is also sanitized; a kernel component reading the trap frame directly is not affected |
| Physical memory writes | range validated, page-bounded, mapped `PAGE_READWRITE|PAGE_NOCACHE`, unmapped immediately | writing a wrong physical page (or MMIO) can corrupt the target or hang the machine; the API is intentionally unrestricted for research use |

Everything the driver changes is recorded and undone by
`DRVLITE_OP_SHUTDOWN` (hooks first, then DSE, then handles/transport). If the
user process dies, kchannel reports `peer-gone` and the driver runs the same
cleanup, but the mapped image remains until the loader unmaps it or the
machine reboots.

## Supported builds and feature detection

| Windows | Driver loads | DSE | InfinityHook / DR-hide | Notes |
|---|---|---|---|---|
| Windows 10 19041 / 19045 x64 | yes | yes (`ci.dll` resolver) | expected; validated on the test VM | no KVA shadow for this CPU class |
| Windows 11 22000 / 22621 / 22631 x64 | yes | yes | expected; layout-checked at install | |
| Windows 11 26100 x64 | yes | yes | validated in the e2e VM | reference build |
| Windows 11 with KVA shadow (`KiSystemCall64Shadow`) | yes | yes | install returns `DRVLITE_ERR_UNSUPPORTED` | trampoline would not be mapped in the shadow address space |
| Windows with HVCI/VBS | memory r/w may still work | may be rejected; reported as unsupported | likely unsupported | do not enable memory integrity for test VMs |
| ARM64 | no | no | no | x64 only |

Feature detection is explicit:
`query_info().features` advertises built capabilities, and the per-operation
calls return `DRVLITE_ERR_UNSUPPORTED` (never a crash) when the running build
does not match. The `InfinityHook` status reply carries the last error code
in `flags`.

## Input validation

- All request headers are magic/version/opcode/size checked
  (`src/core/protocol_codec.cpp`, host-tested).
- All lengths are bounded by `DRVLITE_MAX_TRANSFER`; batch totals are
  overflow-checked; writes must carry exactly `size` bytes.
- All kernel addresses must be canonical kernel addresses, all process
  addresses canonical *user* addresses; page-table entries are validated
  (present bit, large-page bits, physical address mask).
- Process CR3s are validated against `PEB.ImageBaseAddress` before use, so a
  decoy address space is rejected (`DRVLITE_ERR_NOT_FOUND`).
- `drvlite-cli run ... -- fuzz` exercises malformed requests and verifies the
  driver rejects them and stays alive.

## Known non-goals

- No persistence: no service, no registry, no file, no device in the
  production load model.
- No hypervisor hiding: this is the "lite" driver; `blook-drv` owns
  virtualization-level stealth. Anti-cheat/EDR with kernel visibility can
  still detect a mapped image.
- The in-repo `drvlite-mapper.sys` is a **test harness** (device + IOCTL,
  test-signed). Production mapping uses `blook-kdu` or another privileged
  loader; the mapper is never part of the shipped driver.
