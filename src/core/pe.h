/*
 * blook-drv-lite - minimal PE/COFF reader for live kernel images.
 *
 * The driver reads a loaded module's image (e.g. ci.dll) into a pool buffer
 * and resolves exports/offsets from it. Only what that needs is implemented:
 * section table, RVA -> file offset mapping and the export name table.
 */
#ifndef BLOOK_DRVLITE_CORE_PE_H
#define BLOOK_DRVLITE_CORE_PE_H

#include "signature.h"
#include "types.h"

namespace drvlite::core {

constexpr u32 kMaxPeSections = 96;

struct pe_section {
    u64 virtual_address;
    u64 virtual_size;
    u64 raw_offset;
    u64 raw_size;
    char name[9];
};

struct pe_image {
    const u8* data;
    usize size;
    u64 image_base;
    u32 size_of_image;
    u32 size_of_headers;
    u32 number_of_sections;
    u32 export_rva;
    u32 export_size;
    pe_section sections[kMaxPeSections];
};

enum class pe_code : u32 {
    ok = 0,
    too_small,
    bad_dos,
    bad_nt,
    bad_optional,
    bad_sections,
    not_found,
};

const char* pe_code_str(pe_code code);

/* Parses the headers of an image that is mapped at its virtual addresses (a
 * live kernel module). The caller passes the module base separately. */
pe_code pe_parse(const u8* data, usize size, u64 image_base, pe_image* out);

/* Maps an RVA to an offset inside `image->data`. *available receives the
 * number of bytes from that offset to the end of the containing section. */
bool pe_rva_to_offset(const pe_image& image, u32 rva, usize* offset,
                      usize* available);

/* Returns the RVA of an exported symbol, or 0 when it does not exist. */
u32 pe_find_export_rva(const pe_image& image, const char* name);

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_PE_H */
