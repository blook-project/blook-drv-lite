/*
 * blook-drv-lite - handle-table math (layout independent).
 *
 * Windows stores handle table entries as 16-byte records in a one, two or
 * three level table described by HANDLE_TABLE.TableCode. The GrantedAccess
 * field lives inside those 16 bytes as a 25-bit bitfield whose bit offset is
 * not ABI (it moved when entry pointer obfuscation was introduced), so this
 * code does not assume an offset: it is found by matching a known access mask
 * and then rewritten in place.
 */
#ifndef BLOOK_DRVLITE_CORE_HANDLE_TABLE_H
#define BLOOK_DRVLITE_CORE_HANDLE_TABLE_H

#include "types.h"

namespace drvlite::core {

constexpr u32 kHandleEntrySize = 16;
constexpr u32 kHandleTableEntryCount = 512;

/* Reads kernel memory (virtual addresses) through an injected reader. */
using table_read_fn = bool (*)(void* ctx, u64 address, void* buffer,
                               usize size);

struct table_reader {
    void* ctx;
    table_read_fn read;

    bool valid() const { return read != nullptr; }
    bool read_at(u64 address, void* buffer, usize size) const {
        return read != nullptr && read(ctx, address, buffer, size);
    }
};

enum class table_code : u32 {
    ok = 0,
    bad_table,
    out_of_range,
    read_failed,
};

const char* table_code_str(table_code code);

/*
 * Resolves the kernel address of the 16-byte entry for `handle_value` inside
 * the table described by `table_code_value` (HANDLE_TABLE.TableCode).
 *
 * `max_entries` bounds the handle index (e.g. 1 << 22) so a corrupt table
 * code cannot make the caller walk arbitrary memory.
 */
table_code locate_entry(const table_reader& reader, u64 table_code_value,
                        u32 handle_value, u32 max_entries, u64* entry_address);

/*
 * Finds a `access_bits`-wide bitfield equal to `expected_access` inside
 * `entry` and returns its bit offset. Returns false when no unique offset
 * matches (the field is not where the caller expects it, or the entry has
 * been corrupted).
 */
bool find_access_field(const u8* entry, usize entry_size, u32 access_bits,
                       u32 expected_access, u32* bit_offset);

/* Reads a wide bitfield out of a byte buffer. */
u32 get_bits(const u8* data, usize size, u32 bit_offset, u32 bit_count);

/* Writes a wide bitfield into a byte buffer (bits outside stay untouched). */
void set_bits(u8* data, usize size, u32 bit_offset, u32 bit_count, u32 value);

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_HANDLE_TABLE_H */
