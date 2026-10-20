#include "pagewalk.h"

#include "ranges.h"

namespace drvlite::core {
namespace {

/* Offsets inside the PEB (x64, stable since Vista). */
constexpr u64 kPebImageBaseAddress = 0x10;
constexpr u64 kPebLdr = 0x18;

}  // namespace

const char* walk_code_str(walk_code code) {
    switch (code) {
        case walk_code::ok:
            return "ok";
        case walk_code::no_reader:
            return "no-reader";
        case walk_code::non_canonical:
            return "non-canonical";
        case walk_code::not_present:
            return "not-present";
        case walk_code::bad_entry:
            return "bad-entry";
        case walk_code::read_failed:
            return "read-failed";
    }
    return "unknown";
}

walk_code translate(const phys_reader& reader, u64 cr3, u64 virtual_address,
                    translation* out) {
    if (out == nullptr) {
        return walk_code::bad_entry;
    }
    if (!reader.valid()) {
        return walk_code::no_reader;
    }
    if (!is_canonical(virtual_address)) {
        return walk_code::non_canonical;
    }

    const u64 table_base = cr3 & kPageRoundDown;
    u64 entry = 0;

    /* PML4 -> PDPT */
    if (!reader.read_at(table_base + 8 * ((virtual_address >> 39) & 0x1FF),
                        &entry, sizeof(entry))) {
        return walk_code::read_failed;
    }
    if ((entry & kPtePresent) == 0) {
        return walk_code::not_present;
    }

    /* PDPT -> PD, with the 1 GiB page shortcut. */
    if (!reader.read_at(
            (entry & kPteAddressMask) + 8 * ((virtual_address >> 30) & 0x1FF),
            &entry, sizeof(entry))) {
        return walk_code::read_failed;
    }
    if ((entry & kPtePresent) == 0) {
        return walk_code::not_present;
    }
    if ((entry & kPteLarge) != 0) {
        out->physical =
            (entry & kPteLarge1GiBMask) + (virtual_address & (kOneGiB - 1));
        out->span = static_cast<u32>(kOneGiB);
        return walk_code::ok;
    }

    /* PD -> PT, with the 2 MiB page shortcut. */
    if (!reader.read_at(
            (entry & kPteAddressMask) + 8 * ((virtual_address >> 21) & 0x1FF),
            &entry, sizeof(entry))) {
        return walk_code::read_failed;
    }
    if ((entry & kPtePresent) == 0) {
        return walk_code::not_present;
    }
    if ((entry & kPteLarge) != 0) {
        out->physical =
            (entry & kPteLarge2MiBMask) + (virtual_address & (kTwoMiB - 1));
        out->span = static_cast<u32>(kTwoMiB);
        return walk_code::ok;
    }

    /* PT -> 4 KiB page. */
    if (!reader.read_at(
            (entry & kPteAddressMask) + 8 * ((virtual_address >> 12) & 0x1FF),
            &entry, sizeof(entry))) {
        return walk_code::read_failed;
    }
    if ((entry & kPtePresent) == 0) {
        return walk_code::not_present;
    }

    out->physical = (entry & kPteAddressMask) + (virtual_address & kPageMask);
    out->span = static_cast<u32>(kFourKiB);
    return walk_code::ok;
}

read_result read_virtual(const phys_reader& reader, u64 cr3,
                         u64 virtual_address, void* buffer, usize size,
                         bool tolerant) {
    read_result result;
    if (buffer == nullptr && size != 0) {
        result.error = walk_code::bad_entry;
        return result;
    }
    if (!reader.valid()) {
        result.error = walk_code::no_reader;
        return result;
    }

    auto* out = static_cast<u8*>(buffer);
    usize done = 0;
    while (done < size) {
        const u64 current = virtual_address + done;
        translation mapped;
        const walk_code code = translate(reader, cr3, current, &mapped);
        if (code != walk_code::ok) {
            if (result.error == walk_code::ok) {
                result.error = code;
                result.failed_at = current;
            }
            if (!tolerant) {
                /* Report the bytes handed to the caller as zero-filled so a
                 * caller cannot mistake stale buffer contents for data. */
                for (usize index = done; index < size; ++index) {
                    out[index] = 0;
                }
                result.zero_filled += size - done;
                return result;
            }
            const usize step = page_step(current, size - done, size - done);
            for (usize index = 0; index < step; ++index) {
                out[done + index] = 0;
            }
            done += step;
            result.zero_filled += step;
            continue;
        }

        const u64 remaining_in_page =
            mapped.span - (current & (static_cast<u64>(mapped.span) - 1));
        const usize step = static_cast<usize>(remaining_in_page < (size - done)
                                                  ? remaining_in_page
                                                  : (size - done));
        if (!reader.read_at(mapped.physical, out + done, step)) {
            if (result.error == walk_code::ok) {
                result.error = walk_code::read_failed;
                result.failed_at = current;
            }
            if (!tolerant) {
                for (usize index = done; index < size; ++index) {
                    out[index] = 0;
                }
                result.zero_filled += size - done;
                return result;
            }
            for (usize index = 0; index < step; ++index) {
                out[done + index] = 0;
            }
            done += step;
            result.zero_filled += step;
            continue;
        }
        done += step;
        result.copied += step;
    }
    return result;
}

bool validate_cr3(const phys_reader& reader, u64 cr3, u64 peb,
                  u64 expected_image) {
    if (cr3 == 0 || (cr3 & kPageMask) != 0 || !is_canonical_user(peb)) {
        return false;
    }
    u8 header[0x20] = {};
    const read_result read =
        read_virtual(reader, cr3, peb, header, sizeof(header), false);
    if (read.copied != sizeof(header)) {
        return false;
    }
    u64 image_base = 0;
    u64 ldr = 0;
    for (u32 index = 0; index < 8; ++index) {
        image_base |= static_cast<u64>(header[kPebImageBaseAddress + index])
                      << (8 * index);
        ldr |= static_cast<u64>(header[kPebLdr + index]) << (8 * index);
    }
    if (!is_canonical_user(image_base) || !is_canonical_user(ldr)) {
        return false;
    }
    if (expected_image != 0 && image_base != expected_image) {
        return false;
    }
    return true;
}

bool read_u64(const phys_reader& reader, u64 cr3, u64 virtual_address,
              u64* value) {
    if (value == nullptr) {
        return false;
    }
    translation mapped;
    if (translate(reader, cr3, virtual_address, &mapped) != walk_code::ok) {
        return false;
    }
    const u64 remaining =
        mapped.span - (virtual_address & (static_cast<u64>(mapped.span) - 1));
    if (remaining < sizeof(u64)) {
        return false;
    }
    return reader.read_at(mapped.physical, value, sizeof(*value));
}

}  // namespace drvlite::core
