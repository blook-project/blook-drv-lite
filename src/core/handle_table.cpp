#include "handle_table.h"

namespace drvlite::core {

const char* table_code_str(table_code code) {
    switch (code) {
        case table_code::ok:
            return "ok";
        case table_code::bad_table:
            return "bad-table";
        case table_code::out_of_range:
            return "out-of-range";
        case table_code::read_failed:
            return "read-failed";
    }
    return "unknown";
}

table_code locate_entry(const table_reader& reader, u64 table_code_value,
                        u32 handle_value, u32 max_entries, u64* entry_address) {
    if (entry_address == nullptr || !reader.valid()) {
        return table_code::bad_table;
    }
    /* Handle values are 4-byte aligned in the table index space. */
    if ((handle_value & 0x3u) != 0) {
        return table_code::bad_table;
    }
    const u64 index = handle_value >> 2;
    if (max_entries != 0 && index >= max_entries) {
        return table_code::out_of_range;
    }
    const u32 level = static_cast<u32>(table_code_value & 0x3u);
    u64 table = table_code_value & ~0x3ULL;
    if (table == 0 || !is_canonical_kernel(table)) {
        return table_code::bad_table;
    }

    u64 entry_table = table;
    if (level == 0) {
        /* Single level: the table code points straight at the entries. */
        if (index >= kHandleTableEntryCount) {
            return table_code::out_of_range;
        }
    } else if (level == 1) {
        u64 pointer = 0;
        if (!reader.read_at(table + 8 * ((index >> 9) & 0x1FF), &pointer,
                            sizeof(pointer))) {
            return table_code::read_failed;
        }
        if (!is_canonical_kernel(pointer)) {
            return table_code::bad_table;
        }
        entry_table = pointer;
    } else if (level == 2) {
        u64 first = 0;
        u64 second = 0;
        if (!reader.read_at(table + 8 * ((index >> 18) & 0x1FF), &first,
                            sizeof(first)) ||
            !reader.read_at(first + 8 * ((index >> 9) & 0x1FF), &second,
                            sizeof(second))) {
            return table_code::read_failed;
        }
        if (!is_canonical_kernel(first) || !is_canonical_kernel(second)) {
            return table_code::bad_table;
        }
        entry_table = second;
    } else {
        return table_code::bad_table;
    }

    if (!is_canonical_kernel(entry_table)) {
        return table_code::bad_table;
    }
    *entry_address =
        entry_table + (index & 0x1FF) * static_cast<u64>(kHandleEntrySize);
    return table_code::ok;
}

u32 get_bits(const u8* data, usize size, u32 bit_offset, u32 bit_count) {
    if (data == nullptr || bit_count == 0 || bit_count > 32 ||
        static_cast<u64>(bit_offset) + bit_count > size * 8) {
        return 0;
    }
    u32 value = 0;
    for (u32 index = 0; index < bit_count; ++index) {
        const u32 position = bit_offset + index;
        const u32 byte = data[position / 8];
        const u32 bit = (byte >> (position % 8)) & 1u;
        value |= bit << index;
    }
    return value;
}

void set_bits(u8* data, usize size, u32 bit_offset, u32 bit_count, u32 value) {
    if (data == nullptr || bit_count == 0 || bit_count > 32 ||
        static_cast<u64>(bit_offset) + bit_count > size * 8) {
        return;
    }
    for (u32 index = 0; index < bit_count; ++index) {
        const u32 position = bit_offset + index;
        const u32 bit = (value >> index) & 1u;
        u8& byte = data[position / 8];
        const u8 mask = static_cast<u8>(1u << (position % 8));
        if (bit != 0) {
            byte = static_cast<u8>(byte | mask);
        } else {
            byte = static_cast<u8>(byte & ~mask);
        }
    }
}

bool find_access_field(const u8* entry, usize entry_size, u32 access_bits,
                       u32 expected_access, u32* bit_offset) {
    if (entry == nullptr || bit_offset == nullptr || access_bits == 0 ||
        access_bits > 32) {
        return false;
    }
    const u32 total_bits = static_cast<u32>(entry_size * 8);
    if (access_bits > total_bits) {
        return false;
    }
    u32 found = 0;
    u32 found_offset = 0;
    for (u32 offset = 0; offset + access_bits <= total_bits; ++offset) {
        if (get_bits(entry, entry_size, offset, access_bits) ==
            expected_access) {
            ++found;
            found_offset = offset;
            if (found > 1) {
                return false;
            }
        }
    }
    if (found != 1) {
        return false;
    }
    *bit_offset = found_offset;
    return true;
}

}  // namespace drvlite::core
