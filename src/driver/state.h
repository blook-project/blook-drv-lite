/*
 * blook-drv-lite - per-image driver state.
 *
 * The image is mapped exactly once per loader process, so a single global is
 * correct and there is no device object to hang state off. Everything is POD
 * so the image needs no C++ runtime initialisation.
 */
#ifndef BLOOK_DRVLITE_DRIVER_STATE_H
#define BLOOK_DRVLITE_DRIVER_STATE_H

#include "nt.h"

namespace drvlite {

struct dse_state {
    bool supported = false;
    bool patched = false;
    u64 address = 0;
    u64 ci_base = 0;
    u32 ci_size = 0;
    u32 original_value = 0;
    u32 current_value = 0;
    u32 reference_rva = 0;
    u32 via = 0;
    u32 error = 0;
};

struct infinity_state {
    bool installed = false;
    bool supported = false;
    u32 error = 0;
    u64 entry = 0;      /* syscall entry (IA32_LSTAR target) */
    u64 trampoline = 0; /* executable trampoline */
    u64 dispatch = 0;   /* assembly probe stub */
    u32 splice_offset = 0;
    u32 patch_length = 0;
    u64 original_lstar[kMaxProcessors] = {};
    u32 processor_count = 0;
    volatile LONG64 total_syscalls = 0;
    volatile LONG64 filtered_hits = 0;
    u32 filter[DRVLITE_MAX_SSN_FILTER] = {};
    u32 filter_count = 0;
    /* hardware-breakpoint hiding */
    bool hide_enabled = false;
    u32 hide_pid = 0;
    u32 get_context_ssn = 0;
    volatile LONG64 hidden_queries = 0;
    /* syscall shield */
    bool shield_enabled = false;
    u32 shield_pid = 0;
    u32 shield_ssn_count = 0;
    u32 shield_ssns[DRVLITE_MAX_SSN_FILTER] = {};
    volatile LONG64 blocked_calls = 0;
};

struct driver_state {
    const kc_bootstrap* bootstrap = nullptr;
    kc_kernel* kchannel = nullptr;
    u32 client_pid = 0;
    PEPROCESS client_process = nullptr;
    u64 driver_base = 0;
    u32 driver_size = 0;
    u64 kernel_base = 0;
    u32 kernel_size = 0;
    u32 nt_build = 0;
    u32 features = 0;
    u32 handles_open = 0;
    bool shutting_down = false;
    bool cleanup_done = false;
    /* Diagnostic page: first bytes are a magic, its physical address is
     * reported to the SDK so e2e can exercise the physical read/write path. */
    void* test_page = nullptr;
    u64 test_page_pa = 0;
    dse_state dse;
    infinity_state infinity;
    /*
     * Serialises patch installation/removal against shutdown. Handlers run on
     * the single kchannel worker so a spinlock is enough.
     */
    KSPIN_LOCK patch_lock;
};

driver_state& state();

/* Initialisation helpers used by DriverEntry. */
NTSTATUS state_initialize(const kc_bootstrap* bootstrap);
void state_destroy();

/*
 * Restores every kernel modification in reverse order and releases every
 * reference. Idempotent and safe to call from the kchannel worker thread.
 */
void cleanup_all();

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_STATE_H */
