#include "phys.h"

#include "core/ranges.h"

namespace drvlite {
namespace {

bool phys_read_callback(void* context, u64 physical_address, void* buffer,
                        usize size) {
    UNREFERENCED_PARAMETER(context);
    MM_COPY_ADDRESS address{};
    address.PhysicalAddress.QuadPart = static_cast<LONGLONG>(physical_address);
    SIZE_T transferred = 0;
    const NTSTATUS status = MmCopyMemory(buffer, address, size,
                                         kMmCopyMemoryPhysical, &transferred);
    return NT_SUCCESS(status) && transferred == size;
}

/* Maps up to one page at `physical_address` and copies `size` bytes into it
 * (or out of it). The caller keeps the range page bounded. */
bool phys_write_page(u64 physical_address, const void* buffer, usize size) {
    if (size == 0 || size > kPageSize) {
        return false;
    }
    PHYSICAL_ADDRESS base{};
    base.QuadPart = static_cast<LONGLONG>(physical_address & ~kPageMask);
    void* mapping =
        MmMapIoSpaceEx(base, kPageSize, PAGE_READWRITE | PAGE_NOCACHE);
    if (mapping == nullptr) {
        return false;
    }
    const u32 offset = static_cast<u32>(physical_address & kPageMask);
    RtlCopyMemory(static_cast<u8*>(mapping) + offset, buffer, size);
    /* Ensure the write reaches RAM before the mapping disappears (the
     * mapping is non-cached, this is belt and braces). */
    KeMemoryBarrier();
    MmUnmapIoSpace(mapping, kPageSize);
    return true;
}

}  // namespace

core::phys_reader core_phys_reader() {
    core::phys_reader reader;
    reader.ctx = nullptr;
    reader.read = &phys_read_callback;
    return reader;
}

bool phys_read(u64 physical_address, void* buffer, usize size) {
    return phys_read_callback(nullptr, physical_address, buffer, size);
}

bool phys_write(u64 physical_address, const void* buffer, usize size) {
    if (buffer == nullptr) {
        return false;
    }
    const u8* source = static_cast<const u8*>(buffer);
    usize done = 0;
    while (done < size) {
        const usize step =
            core::page_step(physical_address + done, size - done);
        if (!phys_write_page(physical_address + done, source + done, step)) {
            return false;
        }
        done += step;
    }
    return true;
}

}  // namespace drvlite
