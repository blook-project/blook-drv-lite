/*
 * blook-drv-lite - arbitrary process memory through a CR3 page-table walk.
 *
 * The driver never attaches while reading/writing: it resolves the target's
 * CR3, validates it against the process image base (rejecting decoy tables),
 * walks the page tables itself and reads/writes physical memory. The CR3 is
 * resolved on every operation so a changed address space is picked up.
 */
#ifndef BLOOK_DRVLITE_DRIVER_MEM_PROCESS_H
#define BLOOK_DRVLITE_DRIVER_MEM_PROCESS_H

#include "../nt.h"

namespace drvlite {

struct process_ref {
    PEPROCESS process = nullptr;
    u32 pid = 0;
    u64 cr3 = 0;
    u64 peb = 0;
    u64 image_base = 0;
    u32 image_size = 0;
    bool cr3_validated = false;
};

bool process_open(u32 pid, process_ref* out);
void process_close(process_ref* out);

/* Resolves and validates the CR3 of an already opened process. */
bool process_resolve(process_ref* process);

bool process_read(process_ref* process, u64 virtual_address, void* buffer,
                  usize size, bool tolerant, core::read_result* result);

bool process_write(process_ref* process, u64 virtual_address,
                   const void* buffer, usize size);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_MEM_PROCESS_H */
