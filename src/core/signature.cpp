#include "signature.h"

namespace drvlite::core {

bool matches_at(const u8* data, usize size, usize offset,
                const pattern& needle) {
    if (data == nullptr || needle.bytes == nullptr || needle.mask == nullptr) {
        return false;
    }
    if (offset > size || needle.length > size - offset) {
        return false;
    }
    for (usize index = 0; index < needle.length; ++index) {
        const u32 mask = needle.mask[index];
        if ((data[offset + index] & mask) != (needle.bytes[index] & mask)) {
            return false;
        }
    }
    return true;
}

usize find_pattern(const u8* data, usize size, const pattern& needle,
                   usize start) {
    if (data == nullptr || needle.bytes == nullptr || needle.mask == nullptr ||
        needle.length == 0 || needle.length > size) {
        return kNotFound;
    }
    const usize last = size - needle.length;
    for (usize offset = start; offset <= last; ++offset) {
        if (matches_at(data, size, offset, needle)) {
            return offset;
        }
    }
    return kNotFound;
}

namespace {
int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}
}  // namespace

bool parse_pattern(const char* text, u8* bytes_out, u8* mask_out,
                   usize capacity, usize* length_out) {
    if (text == nullptr || bytes_out == nullptr || mask_out == nullptr ||
        length_out == nullptr) {
        return false;
    }
    usize count = 0;
    const char* cursor = text;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count >= capacity) {
            return false;
        }
        if (cursor[0] == '?' && cursor[1] == '?') {
            bytes_out[count] = 0;
            mask_out[count] = 0;
            count += 1;
            cursor += 2;
            continue;
        }
        const int high = hex_value(cursor[0]);
        const int low = hex_value(cursor[1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes_out[count] = static_cast<u8>((high << 4) | low);
        mask_out[count] = 0xFF;
        count += 1;
        cursor += 2;
    }
    *length_out = count;
    return count != 0;
}

}  // namespace drvlite::core
