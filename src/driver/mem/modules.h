/*
 * blook-drv-lite - module enumeration and dumps.
 *
 * Kernel modules come from PsLoadedModuleList; target-process modules come
 * from the PEB loader list walked through the validated CR3.
 */
#ifndef BLOOK_DRVLITE_DRIVER_MODULES_H
#define BLOOK_DRVLITE_DRIVER_MODULES_H

#include "../nt.h"

namespace drvlite {

struct module_entry {
    u64 base;
    u64 size;
    u32 flags;
    u32 name_len;
    char name[DRVLITE_MODULE_NAME_MAX];
};

/* Kernel module list (PsLoadedModuleList). Returns the number written. */
u32 kernel_modules(module_entry* output, u32 capacity);

/* Finds a kernel module by base name ("ci.dll", case insensitive). */
bool kernel_module_find(const char* base_name, u64* base, u32* size);

/* Target process modules through PEB->Ldr (requires the validated CR3). */
u32 process_modules(PEPROCESS process, u64 cr3, module_entry* output,
                    u32 capacity);

/* Dumps `size` bytes at `address` from the kernel (pid == 0) or process
 * address space. Unmapped holes are zero-filled; *copied receives the number
 * of bytes actually read. */
bool module_dump(u32 pid, u64 address, void* output, u32 size, u32* copied);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_MODULES_H */
