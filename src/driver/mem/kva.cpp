#include "kva.h"

#include "phys.h"

namespace drvlite {

u64 system_cr3() {
    return read_cr3();
}

bool read_kernel(u64 virtual_address, void* buffer, usize size) {
    if (buffer == nullptr || !is_kernel_pointer(virtual_address)) {
        return false;
    }
    const core::read_result result = core::read_virtual(
        core_phys_reader(), system_cr3(), virtual_address, buffer, size, false);
    return result.copied == size;
}

bool read_kernel_tolerant(u64 virtual_address, void* buffer, usize size,
                          core::read_result* result) {
    if (buffer == nullptr || !is_kernel_pointer(virtual_address)) {
        if (result != nullptr) {
            *result = core::read_result{};
            result->error = core::walk_code::non_canonical;
        }
        return false;
    }
    const core::read_result value = core::read_virtual(
        core_phys_reader(), system_cr3(), virtual_address, buffer, size, true);
    if (result != nullptr) {
        *result = value;
    }
    return value.copied != 0 || value.zero_filled == size;
}

bool write_kernel(u64 virtual_address, const void* buffer, usize size) {
    if (buffer == nullptr || !is_kernel_pointer(virtual_address)) {
        return false;
    }
    const core::phys_reader reader = core_phys_reader();
    const u64 cr3 = system_cr3();
    const u8* source = static_cast<const u8*>(buffer);
    usize done = 0;
    while (done < size) {
        core::translation mapped;
        if (core::translate(reader, cr3, virtual_address + done, &mapped) !=
            core::walk_code::ok) {
            return false;
        }
        const u64 offset_in_page =
            (virtual_address + done) & (static_cast<u64>(mapped.span) - 1);
        const u64 remaining_in_page = mapped.span - offset_in_page;
        const usize step = static_cast<usize>(remaining_in_page < (size - done)
                                                  ? remaining_in_page
                                                  : (size - done));
        if (!phys_write(mapped.physical, source + done, step)) {
            return false;
        }
        done += step;
    }
    return true;
}

}  // namespace drvlite
