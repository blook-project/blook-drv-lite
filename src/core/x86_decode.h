/*
 * blook-drv-lite - minimal x64 instruction length decoder.
 *
 * Only the lengths and the relocatable fields of compiler generated prologues
 * are needed: detour installation must copy whole instructions and correct
 * RIP-relative displacements. Anything this decoder does not understand
 * fails closed, which for a hook site means "do not patch".
 */
#ifndef BLOOK_DRVLITE_CORE_X86_DECODE_H
#define BLOOK_DRVLITE_CORE_X86_DECODE_H

#include "types.h"

namespace drvlite::core {

struct instruction {
    u32 length = 0;
    u32 rel_offset = 0;  /* offset of a rel8/rel32 operand, 0 when none */
    u32 rel_size = 0;    /* 1 or 4 */
    u32 disp_offset = 0; /* offset of a RIP-relative disp32, 0 when none */
    bool valid = false;
};

/*
 * Decodes one instruction at `code`. `available` bounds the read. Returns
 * false when the encoding is unknown or the instruction does not fit.
 *
 * The decoder understands legacy prefixes, REX, one-byte opcodes, the 0F
 * two-byte map and the 0F38/0F3A three-byte maps with the standard ModRM/
 * SIB/displacement/immediate rules.
 */
bool decode_instruction(const u8* code, usize available, instruction* out);

/* True when the instruction has a field that changes meaning when copied to
 * a different address (relative branch or RIP-relative memory operand). */
bool is_relocatable(const instruction& insn);

/*
 * Copies `length` bytes of instructions from `source` to `destination`,
 * adjusting relative and RIP-relative fields for the different base address.
 * `source_address` and `destination_address` are the virtual addresses the
 * buffers are mapped at. Returns the number of instruction bytes copied, or 0
 * on failure (unknown instruction or destination overlap).
 */
u32 copy_relocated(const u8* source, u64 source_address, u8* destination,
                   u64 destination_address, u32 length,
                   usize destination_capacity);

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_X86_DECODE_H */
