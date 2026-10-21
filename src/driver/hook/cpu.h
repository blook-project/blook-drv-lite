/*
 * blook-drv-lite - cross-processor helpers.
 *
 * Writing IA32_LSTAR must happen on every processor (the MSR is per-CPU).
 * KeIpiGenericCall runs a callback on all online processors with interrupts
 * disabled, which is exactly the serialisation needed.
 */
#ifndef BLOOK_DRVLITE_DRIVER_HOOK_CPU_H
#define BLOOK_DRVLITE_DRIVER_HOOK_CPU_H

#include "../nt.h"

namespace drvlite {

constexpr u32 kLstarMsr = 0xC0000082;

u32 active_processor_count();

bool read_msr_all_cpus(u32 msr, u64* values, u32 capacity,
                       u32* executed = nullptr, u32* debug_processor = nullptr,
                       u64* debug_value = nullptr);

bool write_msr_all_cpus(u32 msr, u64 value);

/* Writes a processor specific value to `msr` on every processor (indexed by
 * KeGetCurrentProcessorNumber). */
bool write_msr_per_cpu(u32 msr, const u64* values, u32 count);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_HOOK_CPU_H */
