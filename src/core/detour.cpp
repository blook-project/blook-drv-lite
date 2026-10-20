#include "detour.h"

namespace drvlite::core {

bool write_absolute_jump(u8* buffer, usize capacity, u64 destination) {
    if (buffer == nullptr || capacity < kAbsoluteJumpLength) {
        return false;
    }
    buffer[0] = 0xFF;
    buffer[1] = 0x25;
    buffer[2] = 0x00;
    buffer[3] = 0x00;
    buffer[4] = 0x00;
    buffer[5] = 0x00;
    for (u32 index = 0; index < 8; ++index) {
        buffer[6 + index] =
            static_cast<u8>((destination >> (8 * index)) & 0xFF);
    }
    return true;
}

bool plan_detour(const u8* target_code, usize available, u64 target_address,
                 u64 detour_address, u64 trampoline_address, detour_plan* out) {
    if (out == nullptr || target_code == nullptr ||
        !is_canonical(target_address) || !is_canonical(detour_address) ||
        !is_canonical(trampoline_address)) {
        return false;
    }
    *out = detour_plan{};

    /* Cover at least the absolute jump with whole instructions. */
    u32 covered = 0;
    while (covered < kAbsoluteJumpLength) {
        instruction insn;
        if (covered >= available ||
            !decode_instruction(target_code + covered, available - covered,
                                &insn)) {
            return false;
        }
        covered += insn.length;
        if (covered > kMaxPatchLength) {
            return false;
        }
    }
    out->patch_length = covered;
    for (u32 index = 0; index < covered; ++index) {
        out->original[index] = target_code[index];
    }
    if (!write_absolute_jump(out->patch, sizeof(out->patch), detour_address)) {
        return false;
    }
    /* Unreachable padding after the jump; int3 keeps a stray fall-through
     * visible instead of silently executing stale bytes. */
    for (u32 index = kAbsoluteJumpLength; index < covered; ++index) {
        out->patch[index] = 0xCC;
    }

    const u32 copied =
        copy_relocated(target_code, target_address, out->trampoline,
                       trampoline_address, covered, sizeof(out->trampoline));
    if (copied != covered) {
        return false;
    }
    if (!write_absolute_jump(out->trampoline + copied,
                             sizeof(out->trampoline) - copied,
                             target_address + covered)) {
        return false;
    }
    out->trampoline_length = copied + kAbsoluteJumpLength;
    return true;
}

}  // namespace drvlite::core
