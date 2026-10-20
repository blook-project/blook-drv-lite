/*
 * blook-drv-lite - physical memory access.
 *
 * Reads use MmCopyMemory(MM_COPY_MEMORY_PHYSICAL). Writes map the page range
 * with MmMapIoSpaceEx(PAGE_READWRITE | PAGE_NOCACHE); non-cached writes are
 * what let the driver modify read-only executable pages (DSE, hooks) without
 * touching CR0.
 */
#ifndef BLOOK_DRVLITE_DRIVER_MEM_PHYS_H
#define BLOOK_DRVLITE_DRIVER_MEM_PHYS_H

#include "../nt.h"

namespace drvlite {

/* core::phys_reader over MmCopyMemory. */
core::phys_reader core_phys_reader();

bool phys_read(u64 physical_address, void* buffer, usize size);

/* Writes `size` bytes at a physical address; page bounded internally. */
bool phys_write(u64 physical_address, const void* buffer, usize size);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_MEM_PHYS_H */
