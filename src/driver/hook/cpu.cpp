#include "cpu.h"

namespace drvlite {
namespace {

struct msr_broadcast {
    u32 msr;
    u32 capacity;
    u64 write_value;
    bool write;
    bool per_cpu;
    const u64* per_cpu_values;
    u64 values[kMaxProcessors];
    volatile LONG executed;
};

ULONG_PTR NTAPI msr_callback(ULONG_PTR context) {
    auto* broadcast = reinterpret_cast<msr_broadcast*>(context);
    const u32 processor = current_processor();
    if (broadcast->write) {
        const u64 value = broadcast->per_cpu
                              ? (processor < broadcast->capacity
                                     ? broadcast->per_cpu_values[processor]
                                     : broadcast->per_cpu_values[0])
                              : broadcast->write_value;
        __writemsr(broadcast->msr, value);
        /* Serialise so the new value is in effect for the next instruction
         * stream on this processor. */
        int registers[4] = {};
        __cpuid(registers, 0);
    } else if (processor < broadcast->capacity) {
        broadcast->values[processor] = __readmsr(broadcast->msr);
    }
    InterlockedIncrement(&broadcast->executed);
    return 0;
}

}  // namespace

u32 active_processor_count() {
    const ULONG count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (count == 0 || count > kMaxProcessors) {
        return 0;
    }
    return static_cast<u32>(count);
}

bool read_msr_all_cpus(u32 msr, u64* values, u32 capacity) {
    if (values == nullptr || capacity == 0 || capacity > kMaxProcessors) {
        return false;
    }
    msr_broadcast broadcast{};
    broadcast.msr = msr;
    broadcast.capacity = capacity;
    broadcast.write = false;
    for (u32 index = 0; index < capacity; ++index) {
        values[index] = 0;
    }
    const ULONG_PTR result = KeIpiGenericCall(
        &msr_callback, reinterpret_cast<ULONG_PTR>(&broadcast));
    UNREFERENCED_PARAMETER(result);
    return broadcast.executed != 0;
}

bool write_msr_all_cpus(u32 msr, u64 value) {
    msr_broadcast broadcast{};
    broadcast.msr = msr;
    broadcast.write = true;
    broadcast.write_value = value;
    broadcast.capacity = 1; /* no per-CPU array needed */
    KeIpiGenericCall(&msr_callback, reinterpret_cast<ULONG_PTR>(&broadcast));
    return broadcast.executed != 0;
}

bool write_msr_per_cpu(u32 msr, const u64* values, u32 count) {
    if (values == nullptr || count == 0 || count > kMaxProcessors) {
        return false;
    }
    msr_broadcast broadcast{};
    broadcast.msr = msr;
    broadcast.write = true;
    broadcast.per_cpu = true;
    broadcast.per_cpu_values = values;
    broadcast.capacity = count;
    KeIpiGenericCall(&msr_callback, reinterpret_cast<ULONG_PTR>(&broadcast));
    return broadcast.executed != 0;
}

}  // namespace drvlite
