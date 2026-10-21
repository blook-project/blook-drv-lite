#include "dse_resolver.h"

#include "signature.h"

namespace drvlite::core {

const char* dse_code_str(dse_code code) {
    switch (code) {
        case dse_code::ok:
            return "ok";
        case dse_code::bad_image:
            return "bad-image";
        case dse_code::no_ci_initialize:
            return "no-ci-initialize";
        case dse_code::no_entry_reference:
            return "no-entry-reference";
        case dse_code::no_options_reference:
            return "no-options-reference";
        case dse_code::target_outside_image:
            return "target-outside-image";
        case dse_code::not_found:
            return "not-found";
    }
    return "unknown";
}

namespace {

constexpr u32 kFunctionScanWindow = 0x200;
constexpr u32 kEntryScanWindow = 0x200;

/* True when the RVA falls inside a section whose name looks like writable
 * data. ci.dll keeps g_CiOptions in .data (or a merge of it). */
bool rva_in_data_section(const pe_image& image, u32 rva) {
    for (u32 index = 0; index < image.number_of_sections; ++index) {
        const pe_section& section = image.sections[index];
        if (rva < section.virtual_address) {
            continue;
        }
        const u64 delta = rva - section.virtual_address;
        const u64 span =
            section.virtual_size != 0 ? section.virtual_size : section.raw_size;
        if (delta >= span) {
            continue;
        }
        const char* name = section.name;
        const char* wanted[] = {".data", "data", ".rdata"};
        for (const char* candidate : wanted) {
            bool equal = true;
            for (usize index2 = 0;; ++index2) {
                if (name[index2] != candidate[index2]) {
                    equal = false;
                    break;
                }
                if (candidate[index2] == '\0') {
                    break;
                }
            }
            if (equal) {
                return true;
            }
        }
        return false;
    }
    return false;
}

/* Decodes the RIP-relative forms that write a global DWORD in ci.dll. */
bool decode_options_write(const u8* data, u64 size, u64 offset, u32* length,
                          u32* via) {
    if (offset + 2 > size) {
        return false;
    }
    const u8 opcode = data[offset];
    const u8 modrm = data[offset + 1];
    if (opcode == 0x89 &&
        (modrm == 0x05 || modrm == 0x0D || modrm == 0x15 || modrm == 0x1D)) {
        *length = 6; /* mov dword ptr [rip+disp32], reg */
        *via = modrm == 0x05 ? 3 : 1;
        return true;
    }
    if (opcode == 0xC7 && modrm == 0x05) {
        *length = 10; /* mov dword ptr [rip+disp32], imm32 */
        *via = 2;
        return true;
    }
    if (opcode == 0x83 && (modrm == 0x05 || modrm == 0x0D)) {
        *length = 7; /* or dword ptr [rip+disp32], imm8 */
        *via = 4;
        return true;
    }
    if (opcode == 0x81 && (modrm == 0x05 || modrm == 0x0D)) {
        *length = 10; /* or dword ptr [rip+disp32], imm32 */
        *via = 5;
        return true;
    }
    return false;
}

bool section_is_text(const pe_section& section) {
    const char* name = section.name;
    for (usize index = 0; name[index] != '\0'; ++index) {
        const char lower = name[index] >= 'A' && name[index] <= 'Z'
                               ? static_cast<char>(name[index] + 32)
                               : name[index];
        if (lower == 't' &&
            (name[index + 1] == 'e' || name[index + 1] == 'E')) {
            return true;
        }
    }
    return false;
}

/* Scans one function body for a direct reference to a global DWORD. */
dse_resolution scan_options_reference(const pe_image& image, u32 function_rva,
                                      u64 image_base) {
    dse_resolution result;
    const u8* data = image.data;
    const usize size = image.size;
    u64 limit = static_cast<u64>(function_rva) + kFunctionScanWindow;
    if (limit > size) {
        limit = size;
    }
    for (u64 offset = function_rva; offset + 6 <= limit; ++offset) {
        u32 instruction_length = 0;
        u32 via = 0;
        if (!decode_options_write(data, size, offset, &instruction_length,
                                  &via)) {
            continue;
        }
        if (offset + instruction_length > size) {
            return result;
        }
        const i32 displacement = load_i32(data + offset + 2);
        const i64 target =
            static_cast<i64>(offset) + instruction_length + displacement;
        if (target <= 0 || static_cast<u64>(target) >= size) {
            continue;
        }
        const u32 target_rva = static_cast<u32>(target);
        if (!rva_in_data_section(image, target_rva)) {
            continue;
        }
        result.code = dse_code::ok;
        result.options_va = image_base + target_rva;
        result.options_rva = target_rva;
        result.reference_rva = static_cast<u32>(offset);
        result.via = via;
        return result;
    }
    result.code = dse_code::no_options_reference;
    return result;
}

}  // namespace

dse_resolution dse_resolve_ci_options(const u8* image, usize size,
                                      u64 image_base) {
    dse_resolution result;
    pe_image parsed;
    if (pe_parse(image, size, image_base, &parsed) != pe_code::ok) {
        result.code = dse_code::bad_image;
        return result;
    }
    const u32 ci_initialize_rva = pe_find_export_rva(parsed, "CiInitialize");
    if (ci_initialize_rva == 0) {
        result.code = dse_code::no_ci_initialize;
        return result;
    }

    /*
     * g_CiOptions is referenced from several ci.dll functions, while most
     * other .data DWORDs are written once. Scan all executable sections,
     * tally references per candidate and prefer the one whose current value
     * looks like the option bitfield (only low bits set) and that is referenced
     * from more than one place. This is robust to encoding changes in
     * CipInitialize across builds (26100 uses different instruction forms).
     */
    struct option_hit {
        u32 rva;
        u32 count;
        u32 via;
        u32 reference;
    };
    constexpr u32 kMaxHits = 64;
    option_hit hits[kMaxHits] = {};
    u32 hit_count = 0;
    for (u32 section_index = 0; section_index < parsed.number_of_sections;
         ++section_index) {
        const pe_section& section = parsed.sections[section_index];
        if (!section_is_text(section)) {
            continue;
        }
        const u64 start = section.virtual_address;
        u64 span =
            section.virtual_size != 0 ? section.virtual_size : section.raw_size;
        if (start >= size) {
            continue;
        }
        if (start + span > size) {
            span = size - start;
        }
        for (u64 offset = start; offset + 10 <= start + span; ++offset) {
            u32 instruction_length = 0;
            u32 via = 0;
            if (!decode_options_write(image, size, offset, &instruction_length,
                                      &via)) {
                continue;
            }
            const i32 displacement = load_i32(image + offset + 2);
            const i64 target =
                static_cast<i64>(offset) + instruction_length + displacement;
            if (target <= 0 || static_cast<u64>(target) + 4 > size) {
                continue;
            }
            const u32 target_rva = static_cast<u32>(target);
            if (!rva_in_data_section(parsed, target_rva)) {
                continue;
            }
            u32 index = 0;
            for (; index < hit_count; ++index) {
                if (hits[index].rva == target_rva) {
                    ++hits[index].count;
                    break;
                }
            }
            if (index == hit_count && hit_count < kMaxHits) {
                hits[hit_count].rva = target_rva;
                hits[hit_count].count = 1;
                hits[hit_count].via = via;
                hits[hit_count].reference = static_cast<u32>(offset);
                ++hit_count;
            }
        }
    }

    i32 best = -1;
    for (u32 index = 0; index < hit_count; ++index) {
        const u32 value = load_u32(image + hits[index].rva);
        const bool plausible = (value & ~0x3Fu) == 0;
        if (!plausible) {
            continue;
        }
        if (best < 0 || hits[index].count > hits[best].count) {
            best = static_cast<i32>(index);
        }
    }
    if (best < 0) {
        for (u32 index = 0; index < hit_count; ++index) {
            if (best < 0 || hits[index].count > hits[best].count) {
                best = static_cast<i32>(index);
            }
        }
    }
    if (best >= 0) {
        result.code = dse_code::ok;
        result.options_rva = hits[best].rva;
        result.options_va = image_base + hits[best].rva;
        result.reference_rva = hits[best].reference;
        result.via = hits[best].via;
        return result;
    }

    /* Fallback: follow CiInitialize to the implementation and scan its body. */
    u64 limit = static_cast<u64>(ci_initialize_rva) + kEntryScanWindow;
    if (limit > size) {
        limit = size;
    }
    for (u64 offset = ci_initialize_rva; offset + 5 <= limit; ++offset) {
        const u8 opcode = image[offset];
        if (opcode != 0xE8 && opcode != 0xE9) {
            continue;
        }
        const i32 displacement = load_i32(image + offset + 1);
        const i64 target = static_cast<i64>(offset) + 5 + displacement;
        if (target <= 0 || static_cast<u64>(target) >= size) {
            continue;
        }
        const dse_resolution candidate = scan_options_reference(
            parsed, static_cast<u32>(target), image_base);
        if (candidate.code == dse_code::ok) {
            return candidate;
        }
    }
    result.code = dse_code::no_entry_reference;
    return result;
}

dse_resolution dse_resolve_legacy_ci_enabled(const u8* image, usize size,
                                             u64 image_base) {
    dse_resolution result;
    if (image == nullptr || size < 0x10) {
        result.code = dse_code::bad_image;
        return result;
    }
    /* `eb 06 1d 88 <rel32>` is the Windows 7 ntoskrnl reference to
     * g_CiEnabled; the target is instruction end + displacement. */
    for (usize offset = 0; offset + 8 <= size; ++offset) {
        if (image[offset] != 0xEB || image[offset + 1] != 0x06 ||
            image[offset + 2] != 0x1D || image[offset + 3] != 0x88) {
            continue;
        }
        const i32 displacement = load_i32(image + offset + 4);
        const i64 target = static_cast<i64>(offset) + 8 + displacement;
        if (target < 0 || static_cast<u64>(target) + 4 > size) {
            continue;
        }
        result.code = dse_code::ok;
        result.options_va = image_base + static_cast<u64>(target);
        result.options_rva = static_cast<u32>(target);
        result.reference_rva = static_cast<u32>(offset);
        result.via = 3;
        return result;
    }
    result.code = dse_code::not_found;
    return result;
}

}  // namespace drvlite::core
