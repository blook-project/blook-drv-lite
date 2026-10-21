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
        const u8 opcode = data[offset];
        const u8 modrm = data[offset + 1];
        u32 instruction_length = 0;
        u32 via = 0;
        /* All RIP-relative forms use mod=00, rm=101 (modrm 0x05/0x0D/0x15/
         * 0x1D); the displacement always starts at offset+2. Windows 10/11
         * CipInitialize historically used rcx/rdx (0x0D/0x15); 26100 can use
         * eax (0x05) or an OR-immediate form, so accept those too. */
        if (opcode == 0x89 && (modrm == 0x05 || modrm == 0x0D ||
                               modrm == 0x15 || modrm == 0x1D)) {
            instruction_length = 6; /* mov dword ptr [rip+disp32], reg */
            via = modrm == 0x05 ? 3 : 1;
        } else if (opcode == 0xC7 && modrm == 0x05) {
            instruction_length = 10; /* mov dword ptr [rip+disp32], imm32 */
            via = 2;
        } else if (opcode == 0x83 && (modrm == 0x05 || modrm == 0x0D)) {
            instruction_length = 7; /* or dword ptr [rip+disp32], imm8 */
            via = 4;
        } else if (opcode == 0x81 && (modrm == 0x05 || modrm == 0x0D)) {
            instruction_length = 10; /* or dword ptr [rip+disp32], imm32 */
            via = 5;
        } else {
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
     * CiInitialize is a thin wrapper: Win7 jumps (E9) straight to
     * CipInitialize, Windows 10/11 sets up the four arguments and calls it
     * (E8). Either way the first relative branch inside the wrapper names the
     * real implementation.
     */
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
        const u32 function_rva = static_cast<u32>(target);
        const dse_resolution candidate =
            scan_options_reference(parsed, function_rva, image_base);
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
