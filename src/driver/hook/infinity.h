/*
 * blook-drv-lite - InfinityHook style system call interception.
 *
 * The syscall entry (`IA32_LSTAR` target) is not patched in code. Instead a
 * trampoline in executable pool memory replays the entry prologue, calls an
 * assembly probe stub before the service dispatch, and resumes the original
 * entry. That keeps the kernel image untouched and makes uninstall a matter of
 * restoring the per-processor MSR.
 *
 * On top of the interception the probe implements:
 *   - system-call accounting and a configurable SSN filter,
 *   - hardware-breakpoint detection bypass: NtGetContextThread is executed
 *     from the probe and its debug-register fields are zeroed for threads of
 *     the protected process,
 *   - a syscall shield: configured SSNs for a configured process are denied
 *     without reaching the original handler.
 */
#ifndef BLOOK_DRVLITE_DRIVER_HOOK_INFINITY_H
#define BLOOK_DRVLITE_DRIVER_HOOK_INFINITY_H

#include "../nt.h"

namespace drvlite {

/* Context filled by the assembly probe stub (keep in sync with
 * syscall_stub.asm). */
struct syscall_probe_context {
    u32 syscall_number;
    u32 reserved;
    u64 target;
    u64 arg0;
    u64 arg1;
    u64 arg2;
    u64 arg3;
    u64 stack_pointer;
    u32 handled;
    u32 status;
};

static_assert(sizeof(syscall_probe_context) == 0x40, "probe context layout");
static_assert(offsetof(syscall_probe_context, target) == 0x08,
              "probe context target offset");
static_assert(offsetof(syscall_probe_context, arg0) == 0x10,
              "probe context arg0 offset");
static_assert(offsetof(syscall_probe_context, stack_pointer) == 0x30,
              "probe context sp offset");
static_assert(offsetof(syscall_probe_context, handled) == 0x38,
              "probe context handled offset");
static_assert(offsetof(syscall_probe_context, status) == 0x3C,
              "probe context status offset");

/* Diagnostic record written into the diagnostic page by
 * infinity_probe_fill(); the CLI `hook-probe` command reads it back to
 * explain why install is (un)supported on this kernel. Keep the layout
 * stable; it is only consumed by the CLI. */
struct infinity_probe_record {
    u32 magic; /* 'PRBH' */
    u32 flags; /* bit0 swapgs, bit1 prefix21, bit2 p1, bit3 p2 */
    u64 kernel_base;
    u64 lstar[8];
    u32 processor_count;
    u32 prefix_match; /* bytes of kEntryPrefix matched */
    u32 p1_offset;    /* first "mov rax,r10; call rax" offset */
    u32 p1_count;
    u32 p2_offset; /* first "mov rax,r10; call r/m" variant offset */
    u32 p2_count;
    u32 p2_modrm; /* modrm byte of the first variant */
    u32 reserved;
    u32 jmp_offset; /* first E9 rel32 in the entry window */
    u32 jmp_ok;
    u64 jmp_target;
    u8 entry_bytes[512];
};
static_assert(sizeof(infinity_probe_record) == 640, "probe record layout");

constexpr u32 kInfinityProbeMagic = 0x48425250u; /* 'PRBH' */

/* Fills `buffer` with an infinity_probe_record; used by the status opcode so
 * the CLI can diagnose unsupported syscall-entry layouts. */
void infinity_probe_fill(void* buffer, usize capacity);

bool infinity_install(const u32* ssn_filter, u32 filter_count);

bool infinity_remove();

void infinity_fill_status(drvlite_infinity_status_rsp* response);

bool infinity_set_hide(bool enable, u32 pid, u32 get_context_ssn);

bool infinity_set_shield(bool enable, u32 pid, const u32* ssns, u32 count);

bool infinity_installed();

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_HOOK_INFINITY_H */
