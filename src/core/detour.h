/*
 * blook-drv-lite - inline-detour planning.
 *
 * A detour overwrites the first whole instructions of a function with an
 * absolute jump to our handler. The overwritten instructions are relocated
 * into a trampoline (placed in executable pool memory) followed by a jump
 * back to the first untouched instruction. The planner is pure and runs on
 * the host in unit tests; the kernel side only allocates the trampoline and
 * applies the bytes with the IPI-synchronised patch primitive.
 */
#ifndef BLOOK_DRVLITE_CORE_DETOUR_H
#define BLOOK_DRVLITE_CORE_DETOUR_H

#include "types.h"
#include "x86_decode.h"

namespace drvlite::core {

/* FF 25 00 00 00 00 <u64 destination> */
constexpr u32 kAbsoluteJumpLength = 14;
constexpr u32 kMaxPatchLength = 32;

struct detour_plan {
    u32 patch_length = 0; /* bytes overwritten at the target */
    u32 trampoline_length = 0;
    u8 patch[kMaxPatchLength] = {};
    u8 original[kMaxPatchLength] = {};
    u8 trampoline[128] = {};
};

/* Writes an absolute indirect jump to `destination` into `buffer`. */
bool write_absolute_jump(u8* buffer, usize capacity, u64 destination);

/*
 * Plans a detour from `target_address` to `detour_address`.
 * `trampoline_address` is where the caller will place the trampoline.
 *
 * Fails (returns false) when the prologue contains an instruction this
 * decoder does not understand, when it needs more than kMaxPatchLength bytes
 * to cover the jump, or when an address is not canonical.
 */
bool plan_detour(const u8* target_code, usize available, u64 target_address,
                 u64 detour_address, u64 trampoline_address, detour_plan* out);

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_DETOUR_H */
