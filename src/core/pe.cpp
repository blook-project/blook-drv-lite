#include "pe.h"

namespace drvlite::core {

const char* pe_code_str(pe_code code) {
    switch (code) {
        case pe_code::ok:
            return "ok";
        case pe_code::too_small:
            return "too-small";
        case pe_code::bad_dos:
            return "bad-dos";
        case pe_code::bad_nt:
            return "bad-nt";
        case pe_code::bad_optional:
            return "bad-optional";
        case pe_code::bad_sections:
            return "bad-sections";
        case pe_code::not_found:
            return "not-found";
    }
    return "unknown";
}

namespace {

constexpr u16 kDosMagic = 0x5A4D;        /* 'MZ' */
constexpr u32 kNtSignature = 0x00004550; /* 'PE\0\0' */
constexpr u16 kOptionalMagic64 = 0x20B;
constexpr u16 kOptionalMagic32 = 0x10B;

u16 load_u16_at(const u8* data, usize size, usize offset) {
    if (offset + 2 > size) {
        return 0;
    }
    return load_u16(data + offset);
}

u32 load_u32_at(const u8* data, usize size, usize offset) {
    if (offset + 4 > size) {
        return 0;
    }
    return load_u32(data + offset);
}

bool copy_cstr(const u8* data, usize size, usize offset, char* out,
               usize capacity) {
    if (capacity == 0 || offset >= size) {
        return false;
    }
    usize count = 0;
    while (offset + count < size && count + 1 < capacity) {
        const char value = static_cast<char>(data[offset + count]);
        out[count] = value;
        ++count;
        if (value == '\0') {
            return true;
        }
    }
    out[count < capacity ? count : capacity - 1] = '\0';
    return false;
}

}  // namespace

/*
 * `data` is a live mapped image: its byte offset for an RVA is the RVA itself.
 * File-layout (raw offset) mapping is not needed by this project.
 */
pe_code pe_parse(const u8* data, usize size, u64 image_base, pe_image* out) {
    if (out == nullptr) {
        return pe_code::bad_optional;
    }
    *out = pe_image{};
    if (data == nullptr || size < 0x40) {
        return pe_code::too_small;
    }
    out->data = data;
    out->size = size;
    out->image_base = image_base;

    if (load_u16(data) != kDosMagic) {
        return pe_code::bad_dos;
    }
    const u32 nt_offset = load_u32_at(data, size, 0x3C);
    if (nt_offset < 0x40 || nt_offset + 24 > size) {
        return pe_code::bad_nt;
    }
    if (load_u32_at(data, size, nt_offset) != kNtSignature) {
        return pe_code::bad_nt;
    }
    const u16 number_of_sections = load_u16_at(data, size, nt_offset + 6);
    const u16 optional_size = load_u16_at(data, size, nt_offset + 20);
    const usize optional_offset = static_cast<usize>(nt_offset) + 24;
    if (optional_size < 0x70 || optional_offset + optional_size > size) {
        return pe_code::bad_optional;
    }
    const u16 magic = load_u16_at(data, size, optional_offset);
    if (magic != kOptionalMagic64 && magic != kOptionalMagic32) {
        return pe_code::bad_optional;
    }
    out->size_of_image = load_u32_at(data, size, optional_offset + 56);
    out->size_of_headers = load_u32_at(data, size, optional_offset + 60);
    const usize directories =
        optional_offset + (magic == kOptionalMagic64 ? 112 : 96);
    if (directories + 8 <= optional_offset + optional_size) {
        out->export_rva = load_u32_at(data, size, directories);
        out->export_size = load_u32_at(data, size, directories + 4);
    }

    if (number_of_sections > kMaxPeSections) {
        return pe_code::bad_sections;
    }
    out->number_of_sections = number_of_sections;
    const usize section_table = optional_offset + optional_size;
    for (u16 index = 0; index < number_of_sections; ++index) {
        const usize entry = section_table + static_cast<usize>(index) * 40;
        if (entry + 40 > size) {
            return pe_code::bad_sections;
        }
        pe_section& section = out->sections[index];
        for (usize name_index = 0; name_index < 8; ++name_index) {
            section.name[name_index] =
                static_cast<char>(data[entry + name_index]);
        }
        section.name[8] = '\0';
        section.virtual_size = load_u32_at(data, size, entry + 8);
        section.virtual_address = load_u32_at(data, size, entry + 12);
        section.raw_size = load_u32_at(data, size, entry + 16);
        section.raw_offset = load_u32_at(data, size, entry + 20);
    }
    return pe_code::ok;
}

bool pe_rva_to_offset(const pe_image& image, u32 rva, usize* offset,
                      usize* available) {
    if (offset == nullptr || image.data == nullptr || rva >= image.size) {
        return false;
    }
    *offset = rva;
    if (available != nullptr) {
        *available = static_cast<usize>(image.size - rva);
    }
    return true;
}

u32 pe_find_export_rva(const pe_image& image, const char* name) {
    if (name == nullptr || image.export_rva == 0 || image.data == nullptr) {
        return 0;
    }
    usize export_offset = 0;
    if (!pe_rva_to_offset(image, image.export_rva, &export_offset, nullptr)) {
        return 0;
    }
    if (export_offset + 40 > image.size) {
        return 0;
    }
    const u8* data = image.data;
    const u32 number_of_functions =
        load_u32_at(data, image.size, export_offset + 20);
    const u32 number_of_names =
        load_u32_at(data, image.size, export_offset + 24);
    const u32 functions_rva = load_u32_at(data, image.size, export_offset + 28);
    const u32 names_rva = load_u32_at(data, image.size, export_offset + 32);
    const u32 ordinals_rva = load_u32_at(data, image.size, export_offset + 36);
    if (functions_rva == 0 || names_rva == 0 || ordinals_rva == 0 ||
        number_of_names == 0 || number_of_names > 65536 ||
        number_of_functions > 65536) {
        return 0;
    }
    for (u32 index = 0; index < number_of_names; ++index) {
        const usize name_entry =
            static_cast<usize>(names_rva) + static_cast<usize>(index) * 4;
        const usize ordinal_entry =
            static_cast<usize>(ordinals_rva) + static_cast<usize>(index) * 2;
        if (name_entry + 4 > image.size || ordinal_entry + 2 > image.size) {
            return 0;
        }
        const u32 symbol_rva = load_u32(data + name_entry);
        char symbol[128] = {};
        if (!copy_cstr(data, image.size, symbol_rva, symbol, sizeof(symbol))) {
            continue;
        }
        bool equal = true;
        for (usize char_index = 0;; ++char_index) {
            if (symbol[char_index] != name[char_index]) {
                equal = false;
                break;
            }
            if (name[char_index] == '\0') {
                break;
            }
        }
        if (!equal) {
            continue;
        }
        const u16 ordinal = load_u16(data + ordinal_entry);
        if (ordinal >= number_of_functions) {
            return 0;
        }
        const usize function_entry =
            static_cast<usize>(functions_rva) + static_cast<usize>(ordinal) * 4;
        if (function_entry + 4 > image.size) {
            return 0;
        }
        return load_u32(data + function_entry);
    }
    return 0;
}

}  // namespace drvlite::core
