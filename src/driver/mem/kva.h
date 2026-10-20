/*
 * blook-drv-lite - kernel virtual memory access.
 *
 * All access goes through the page tables of the current (system) address
 * space and then physical memory, so non-present pages, large pages and
 * read-only pages are handled uniformly and an invalid address returns an
 * error instead of faulting.
 */
#ifndef BLOOK_DRVLITE_DRIVER_MEM_KVA_H
#define BLOOK_DRVLITE_DRIVER_MEM_KVA_H

#include "../nt.h"

namespace drvlite {

/* CR3 of the current kernel address space (worker thread: System process). */
u64 system_cr3();

bool read_kernel(u64 virtual_address, void* buffer, usize size);

bool read_kernel_tolerant(u64 virtual_address, void* buffer, usize size,
                          core::read_result* result);

bool write_kernel(u64 virtual_address, const void* buffer, usize size);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_MEM_KVA_H */
