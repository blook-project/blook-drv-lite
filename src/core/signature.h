/*
 * blook-drv-lite - byte pattern scanning over an in-memory image.
 *
 * Used to resolve ci.dll!g_CiOptions and other version dependent constants
 * from live kernel images. The image is just a buffer: unit tests inject
 * synthetic images, the driver injects a copy of the loaded module.
 */
#ifndef BLOOK_DRVLITE_CORE_SIGNATURE_H
#define BLOOK_DRVLITE_CORE_SIGNATURE_H

#include "types.h"

namespace drvlite::core {

/* A byte pattern where `mask[i] == 0xFF` requires bytes[i] and `mask[i] == 0`
 * is a wildcard (0x00); intermediate values compare only the masked bits. */
struct pattern {
    const u8* bytes;
    const u8* mask;
    usize length;
};

bool matches_at(const u8* data, usize size, usize offset,
                const pattern& needle);

/* Returns the first matching offset, or kNotFound. */
constexpr usize kNotFound = static_cast<usize>(-1);

usize find_pattern(const u8* data, usize size, const pattern& needle,
                   usize start = 0);

/* Little-endian readers used by the resolvers (safe when `size` is known). */
inline i32 load_i32(const u8* data) {
    return static_cast<i32>(
        static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
        (static_cast<u32>(data[2]) << 16) | (static_cast<u32>(data[3]) << 24));
}

inline u16 load_u16(const u8* data) {
    return static_cast<u16>(static_cast<u16>(data[0]) |
                            (static_cast<u16>(data[1]) << 8));
}

inline u32 load_u32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) |
           (static_cast<u32>(data[3]) << 24);
}

inline u64 load_u64(const u8* data) {
    return static_cast<u64>(load_u32(data)) |
           (static_cast<u64>(load_u32(data + 4)) << 32);
}

/* Builds a mask from a literal signature string where '?' matches any byte,
 * e.g. "48 8B 05 ? ? ? ? 48 85 C0". Returns false on a malformed string. */
bool parse_pattern(const char* text, u8* bytes_out, u8* mask_out,
                   usize capacity, usize* length_out);

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_SIGNATURE_H */
