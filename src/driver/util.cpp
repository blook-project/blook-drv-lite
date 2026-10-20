#include "util.h"

#include "state.h"

namespace drvlite {
namespace {

constexpr u16 kDosMagic = 0x5A4D;
constexpr u32 kNtSignature = 0x00004550;
constexpr u64 kMaxImageScan = 32ULL << 20;

bool valid_pe_at(u64 base, const void* entry_point) {
    __try {
        const u16 dos = *reinterpret_cast<const u16*>(base);
        if (dos != kDosMagic) {
            return false;
        }
        const u32 nt_offset = *reinterpret_cast<const u32*>(base + 0x3C);
        if (nt_offset < 0x40 || nt_offset > 0x1000) {
            return false;
        }
        const u32 signature = *reinterpret_cast<const u32*>(base + nt_offset);
        if (signature != kNtSignature) {
            return false;
        }
        const u16 optional_size =
            *reinterpret_cast<const u16*>(base + nt_offset + 20);
        const u16 entry_offset =
            *reinterpret_cast<const u16*>(base + nt_offset + 16);
        const u32 entry_rva =
            *reinterpret_cast<const u32*>(base + nt_offset + 24 + 16);
        (void)optional_size;
        if (entry_offset != 0x10) {
            return false;
        }
        const u64 expected_entry = base + entry_rva;
        if (reinterpret_cast<u64>(entry_point) != expected_entry) {
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

u64 image_base_from_pointer(const void* address) {
    if (address == nullptr) {
        return 0;
    }
    u64 cursor = reinterpret_cast<u64>(address) & ~0xFFFULL;
    const u64 limit = cursor > kMaxImageScan ? cursor - kMaxImageScan : 0;
    while (cursor > limit) {
        if (valid_pe_at(cursor, address)) {
            return cursor;
        }
        cursor -= 0x1000;
    }
    return 0;
}

u32 image_size_at(u64 base) {
    __try {
        if (*reinterpret_cast<const u16*>(base) != kDosMagic) {
            return 0;
        }
        const u32 nt_offset = *reinterpret_cast<const u32*>(base + 0x3C);
        if (nt_offset < 0x40 || nt_offset > 0x1000) {
            return 0;
        }
        if (*reinterpret_cast<const u32*>(base + nt_offset) != kNtSignature) {
            return 0;
        }
        return *reinterpret_cast<const u32*>(base + nt_offset + 24 + 56);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

char ascii_lower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

bool unicode_equals_ascii(const UNICODE_STRING& value, const char* ascii) {
    if (value.Buffer == nullptr || ascii == nullptr) {
        return false;
    }
    const usize length = value.Length / sizeof(WCHAR);
    usize index = 0;
    for (; index < length; ++index) {
        if (ascii[index] == '\0') {
            return false;
        }
        const wchar_t left = static_cast<wchar_t>(value.Buffer[index]);
        if (left > 0x7F ||
            ascii_lower(static_cast<char>(left)) != ascii_lower(ascii[index])) {
            return false;
        }
    }
    return ascii[index] == '\0';
}

void narrow_copy(const UNICODE_STRING& value, char* output, usize capacity) {
    if (output == nullptr || capacity == 0) {
        return;
    }
    usize index = 0;
    const usize length = value.Length / sizeof(WCHAR);
    for (; index < length && index + 1 < capacity; ++index) {
        wchar_t character = value.Buffer[index];
        if (character > 0x7F) {
            character = '?';
        }
        output[index] = ascii_lower(static_cast<char>(character));
    }
    output[index] = '\0';
}

bool safe_read_kernel(u64 address, void* buffer, usize size) {
    if (buffer == nullptr || !is_kernel_pointer(address)) {
        return false;
    }
    return read_kernel(address, buffer, size);
}

}  // namespace drvlite
