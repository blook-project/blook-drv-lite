#include "x86_decode.h"

#include "signature.h"

namespace drvlite::core {
namespace {

/* Prefix bytes that never terminate an instruction. */
bool is_legacy_prefix(u8 value) {
    switch (value) {
        case 0x26:
        case 0x2E:
        case 0x36:
        case 0x3E:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
        case 0xF0:
        case 0xF2:
        case 0xF3:
            return true;
        default:
            return false;
    }
}

/* Returns the number of ModRM/SIB/displacement bytes for `modrm`, writing the
 * displacement offset (relative to `modrm_offset`) into `disp_offset`. */
u32 decode_modrm(const u8* code, usize available, u32 modrm_offset,
                 u32* disp_offset, bool* rip_relative) {
    if (modrm_offset >= available) {
        return 0;
    }
    const u8 modrm = code[modrm_offset];
    const u32 mod = modrm >> 6;
    const u32 rm = modrm & 0x7;
    u32 extra = 1; /* the ModRM byte itself */
    u32 disp_size = 0;

    if (mod != 3) {
        if (rm == 4) {
            if (modrm_offset + extra >= available) {
                return 0;
            }
            const u8 sib = code[modrm_offset + extra];
            ++extra;
            if (mod == 0 && (sib & 0x7) == 5) {
                disp_size = 4;
            }
        } else if (mod == 0 && rm == 5) {
            disp_size = 4;
            *rip_relative = true;
        }
        if (mod == 1) {
            disp_size = 1;
        } else if (mod == 2) {
            disp_size = 4;
        }
        if (disp_size != 0) {
            if (modrm_offset + extra + disp_size > available) {
                return 0;
            }
            *disp_offset = modrm_offset + extra;
            extra += disp_size;
        }
    }
    return extra;
}

/* Immediate size for the one-byte opcode map. `rex_w` and `operand16` adjust
 * the 16/32/64-bit cases. Returns kUnknown for encodings this decoder does
 * not support (fails closed). */
constexpr u32 kUnknown = 0xFF;
constexpr u32 kNoImmediate = 0;

u32 one_byte_immediate(u8 opcode, bool rex_w, bool operand16) {
    if (opcode >= 0x00 && opcode <= 0x3F) {
        const u8 low = opcode & 0x7;
        if (low == 4) {
            return 1;
        }
        if (low == 5) {
            return operand16 ? 2 : 4;
        }
        return kNoImmediate; /* low 0..3: ModRM form */
    }
    if (opcode >= 0x70 && opcode <= 0x7F) {
        return 1; /* rel8 */
    }
    switch (opcode) {
        case 0x68:
            return operand16 ? 2 : 4;
        case 0x69:
            return operand16 ? 2 : 4;
        case 0x6A:
            return 1;
        case 0x6B:
            return 1;
        case 0x80:
        case 0x82:
        case 0x83:
        case 0xC0:
        case 0xC1:
        case 0xC6:
            return 1;
        case 0x81:
            return operand16 ? 2 : 4;
        case 0xC7:
            return operand16 ? 2 : 4;
        case 0xA0:
        case 0xA1:
        case 0xA2:
        case 0xA3:
            return 8; /* moffs64 */
        case 0xA8:
            return 1;
        case 0xA9:
            return operand16 ? 2 : 4;
        case 0xB0:
        case 0xB1:
        case 0xB2:
        case 0xB3:
        case 0xB4:
        case 0xB5:
        case 0xB6:
        case 0xB7:
            return 1;
        case 0xB8:
        case 0xB9:
        case 0xBA:
        case 0xBB:
        case 0xBC:
        case 0xBD:
        case 0xBE:
        case 0xBF:
            if (rex_w) {
                return 8;
            }
            return operand16 ? 2 : 4;
        case 0xC2:
        case 0xCA:
            return 2;
        case 0xCD:
            return 1;
        case 0xE4:
        case 0xE5:
        case 0xE6:
        case 0xE7:
            return 1;
        case 0xE8:
        case 0xE9:
            return 4;
        case 0xEB:
            return 1;
        case 0xF6:
        case 0xF7:
            return kUnknown; /* depends on the ModRM reg field */
        case 0xC8:
            return 3; /* enter imm16, imm8 */
        default:
            return kNoImmediate;
    }
}

bool one_byte_has_modrm(u8 opcode) {
    if (opcode <= 0x3F) {
        const u8 low = opcode & 0x7;
        /* 0x26/0x2E/0x36/0x3E are prefixes; 0x27/0x2F/0x37/0x3F invalid. */
        return low <= 3 || opcode == 0x63;
    }
    if (opcode >= 0x50 && opcode <= 0x5F) {
        return false;
    }
    if (opcode >= 0x70 && opcode <= 0x7F) {
        return false;
    }
    if (opcode >= 0x90 && opcode <= 0x9F) {
        return false;
    }
    switch (opcode) {
        case 0x63:
        case 0x69:
        case 0x6B:
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F:
        case 0xC0:
        case 0xC1:
        case 0xC6:
        case 0xC7:
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
        case 0xD8:
        case 0xD9:
        case 0xDA:
        case 0xDB:
        case 0xDC:
        case 0xDD:
        case 0xDE:
        case 0xDF:
        case 0xF6:
        case 0xF7:
        case 0xFE:
        case 0xFF:
            return true;
        default:
            return false;
    }
}

/* Immediate size for the 0F map: 0 for none, 1 for imm8, 4 for imm32. */
u32 two_byte_immediate(u8 opcode) {
    switch (opcode) {
        case 0x70:
        case 0x71:
        case 0x72:
        case 0x73:
        case 0xA4:
        case 0xAC:
        case 0xBA:
        case 0xC2:
        case 0xC4:
        case 0xC5:
        case 0xC6:
            return 1;
        case 0x0F:
            return 1; /* 3DNow: ModRM + imm8 */
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F:
            return 4; /* jcc rel32 */
        case 0xA0:
        case 0xA1:
        case 0xA2:
        case 0xA8:
        case 0xA9:
        case 0xAA:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0B:
        case 0x0E:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35:
        case 0x37:
        case 0x77:
        case 0xB9:
        case 0xFF:
            return 0;
        default:
            return 0;
    }
}

bool two_byte_has_modrm(u8 opcode) {
    if (opcode >= 0x80 && opcode <= 0x8F) {
        return false; /* jcc rel32 */
    }
    switch (opcode) {
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0B:
        case 0x0E:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35:
        case 0x37:
        case 0x77:
        case 0xA0:
        case 0xA1:
        case 0xA2:
        case 0xA8:
        case 0xA9:
        case 0xAA:
        case 0xFF:
            return false;
        default:
            return true;
    }
}

}  // namespace

bool decode_instruction(const u8* code, usize available, instruction* out) {
    if (out == nullptr || code == nullptr) {
        return false;
    }
    *out = instruction{};
    usize offset = 0;
    bool operand16 = false;
    bool rex_w = false;

    while (offset < available) {
        const u8 value = code[offset];
        if (is_legacy_prefix(value)) {
            if (value == 0x66) {
                operand16 = true;
            }
            ++offset;
            continue;
        }
        if ((value & 0xF0) == 0x40) { /* REX */
            rex_w = (value & 0x08) != 0;
            ++offset;
            continue;
        }
        break;
    }
    if (offset >= available) {
        return false;
    }
    /* VEX/EVEX encoded instructions are intentionally unsupported: hook sites
     * are function prologues and failing closed is the safe behaviour. */
    const u8 opcode = code[offset++];
    if (opcode == 0xC4 || opcode == 0xC5 || opcode == 0x62) {
        return false;
    }

    bool has_modrm = false;
    u32 immediate = kNoImmediate;
    if (opcode == 0x0F) {
        if (offset >= available) {
            return false;
        }
        const u8 opcode2 = code[offset++];
        if (opcode2 == 0x38 || opcode2 == 0x3A) {
            if (offset >= available) {
                return false;
            }
            const u8 opcode3 = code[offset++];
            (void)opcode3;
            has_modrm = true;
            immediate = opcode2 == 0x3A ? 1 : kNoImmediate;
        } else {
            has_modrm = two_byte_has_modrm(opcode2);
            immediate = two_byte_immediate(opcode2);
            if (opcode2 >= 0x80 && opcode2 <= 0x8F) {
                out->rel_offset = static_cast<u32>(offset);
                out->rel_size = 4;
            }
        }
    } else {
        has_modrm = one_byte_has_modrm(opcode);
        immediate = one_byte_immediate(opcode, rex_w, operand16);
        if (opcode == 0xE8 || opcode == 0xE9) {
            out->rel_offset = static_cast<u32>(offset);
            out->rel_size = 4;
        } else if (opcode == 0xEB || (opcode >= 0x70 && opcode <= 0x7F) ||
                   (opcode >= 0xE0 && opcode <= 0xE3)) {
            out->rel_offset = static_cast<u32>(offset);
            out->rel_size = 1;
            immediate = 1;
        }
        if (opcode == 0xF6 || opcode == 0xF7) {
            if (offset >= available) {
                return false;
            }
            const u8 modrm = code[offset];
            const u8 reg = (modrm >> 3) & 0x7;
            /* TEST (reg 0/1) has an immediate; the other groups do not. */
            if (reg <= 1) {
                immediate = opcode == 0xF6 ? 1 : (operand16 ? 2 : 4);
            } else {
                immediate = kNoImmediate;
            }
        }
    }
    if (immediate == kUnknown) {
        return false;
    }

    if (has_modrm) {
        u32 disp_offset = 0;
        bool rip_relative = false;
        const u32 modrm_length =
            decode_modrm(code, available, static_cast<u32>(offset),
                         &disp_offset, &rip_relative);
        if (modrm_length == 0) {
            return false;
        }
        if (rip_relative) {
            out->disp_offset = disp_offset;
        }
        offset += modrm_length;
    }
    if (immediate != kNoImmediate) {
        if (offset + immediate > available) {
            return false;
        }
        offset += immediate;
    }
    /* A relative operand that was not recorded above (ModRM branches are not
     * relative) leaves 0. */
    if (out->rel_offset != 0 && out->rel_offset + out->rel_size > offset) {
        return false;
    }
    if (out->disp_offset != 0 && out->disp_offset + 4 > offset) {
        return false;
    }
    if (offset > 15) {
        return false;
    }
    out->length = static_cast<u32>(offset);
    out->valid = true;
    return true;
}

bool is_relocatable(const instruction& insn) {
    return insn.rel_offset != 0 || insn.disp_offset != 0;
}

namespace {

bool fits_i32(i64 value) {
    return value >= -0x80000000LL && value <= 0x7FFFFFFFLL;
}

bool fits_i8(i64 value) {
    return value >= -128 && value <= 127;
}

void store_u32(u8* destination, u32 value) {
    for (u32 index = 0; index < 4; ++index) {
        destination[index] = static_cast<u8>((value >> (8 * index)) & 0xFF);
    }
}

}  // namespace

u32 copy_relocated(const u8* source, u64 source_address, u8* destination,
                   u64 destination_address, u32 length,
                   usize destination_capacity) {
    if (source == nullptr || destination == nullptr ||
        length > destination_capacity) {
        return 0;
    }
    u32 copied = 0;
    while (copied < length) {
        instruction insn;
        if (!decode_instruction(source + copied, length - copied, &insn)) {
            return 0;
        }
        const u64 source_insn = source_address + copied;
        const u64 destination_insn = destination_address + copied;
        for (u32 index = 0; index < insn.length; ++index) {
            destination[copied + index] = source[copied + index];
        }
        if (insn.rel_offset != 0) {
            const i64 target =
                insn.rel_size == 4
                    ? static_cast<i64>(source_insn) + insn.length +
                          load_i32(source + copied + insn.rel_offset)
                    : static_cast<i64>(source_insn) + insn.length +
                          static_cast<i8>(source[copied + insn.rel_offset]);
            const i64 adjusted =
                target - (static_cast<i64>(destination_insn) + insn.length);
            if (insn.rel_size == 4) {
                if (!fits_i32(adjusted)) {
                    return 0; /* rel32 cannot reach the relocated target */
                }
                store_u32(destination + copied + insn.rel_offset,
                          static_cast<u32>(adjusted));
            } else {
                if (!fits_i8(adjusted)) {
                    return 0;
                }
                destination[copied + insn.rel_offset] =
                    static_cast<u8>(static_cast<i8>(adjusted));
            }
        }
        if (insn.disp_offset != 0) {
            const i64 target = static_cast<i64>(source_insn) + insn.length +
                               load_i32(source + copied + insn.disp_offset);
            const i64 adjusted =
                target - (static_cast<i64>(destination_insn) + insn.length);
            if (!fits_i32(adjusted)) {
                return 0; /* RIP-relative operand cannot reach */
            }
            store_u32(destination + copied + insn.disp_offset,
                      static_cast<u32>(adjusted));
        }
        copied += insn.length;
    }
    return copied;
}

}  // namespace drvlite::core
