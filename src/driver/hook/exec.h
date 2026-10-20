/*
 * blook-drv-lite - executable pool allocation for the syscall trampoline.
 */
#ifndef BLOOK_DRVLITE_DRIVER_HOOK_EXEC_H
#define BLOOK_DRVLITE_DRIVER_HOOK_EXEC_H

#include "../nt.h"

namespace drvlite {

constexpr u64 kExecPoolTag = 'sxdD';

/* Allocates executable non-paged memory, or returns nullptr when the platform
 * refuses executable pool (feature-detectable by callers). */
void* exec_alloc(usize size);

void exec_free(void* address);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_HOOK_EXEC_H */
