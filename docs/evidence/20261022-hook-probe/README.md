# InfinityHook on 26100 with KVA shadow: probe findings

Captured from `drvlite-cli run … hook-probe` inside the bvmenv guest
(`vm-drv-lite`, Windows 11 26100, 4 vCPU). Raw output: `hook-probe.txt`.

## What the probe shows

- `IA32_LSTAR` points at `0xfffff800d30b5240`, inside ntoskrnl: this is
  `KiSystemCall64Shadow` (KVA shadow / KPTI is active on this guest).
- The shadow entry prologue matches the 21-byte swapgs/GsSwitch prefix exactly
  (`prefix=21/21`), so the old prefix check is not the blocker.
- The service dispatch pattern `49 8B C2 FF D0` (`mov rax, r10; call rax`) does
  **not** occur in the LSTAR stub (`p1 count=0`), so the existing splice check
  fails and install returns `DRVLITE_ERR_UNSUPPORTED` (16), fail-closed.
- The shared (post-CR3-switch) entry does contain the pattern: 3 occurrences in
  ntoskrnl, and the nearest one after the LSTAR target is at
  `0xfffff800d30b5750` (`entry + 0x510`), i.e. `KiSystemServiceStart`.
- Per-CPU MSR reads are unreliable in this guest: `executed=4` but the values
  array is zero, while a direct current-CPU `__readmsr(LSTAR)` is valid. The
  install path now reads the current processor and reuses that value for all
  processors.

## Prototype attempted and why it is disabled

A dispatch-inline-hook fallback was implemented: patch the shared
`mov rax, r10; call rax` with an absolute jump to the probe stub, trampoline
replays the original 5 bytes and returns to `shared + 5` on the chain path.
On this shadow layout the guest hangs: the assembly probe frame offsets
(`rsp+0xB0` etc. in `syscall_stub.asm`) assume the non-shadow trap frame;
after the shadow prologue's extra `sub rsp, 158h` frame they point at the
wrong slots, and every syscall goes through the patched path. The fallback is
therefore compiled out behind `kShadowDispatchHook = false`; install remains
fail-closed with `DRVLITE_ERR_UNSUPPORTED` and the e2e/CI matrices record
`KVA-shadow platform` skips for `infinityhook_counters`,
`hwbp_detection_bypass` and `shutdown_with_hooks_armed`.

## Next step (not in the 1.0.3 window)

Rework `syscall_stub.asm` frame discovery for the shadow layout (derive the
offsets from the actual prologue instead of hard-coded `rsp+0xB0`/`0xE0`),
then re-enable `kShadowDispatchHook` and validate `hook-test` counters plus
`hwbp-test` Dr0 hiding in the VM.
