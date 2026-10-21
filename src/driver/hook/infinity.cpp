#include "infinity.h"

#include "../mem/kva.h"
#include "../mem/modules.h"
#include "../state.h"
#include "cpu.h"
#include "exec.h"

extern "C" {
void drvlite_syscall_entry_stub();
extern unsigned long long g_drvlite_infinity_resume;
extern unsigned long long g_drvlite_infinity_skip;
}

extern "C" {
unsigned long long g_drvlite_infinity_resume = 0;
unsigned long long g_drvlite_infinity_skip = 0;
}

namespace drvlite {
namespace {

/* mov rax, r10; call rax - the service dispatch of the supported builds. */
const u8 kDispatchPattern[] = {0x49, 0x8B, 0xC2, 0xFF, 0xD0};
constexpr u32 kDispatchPatternLength = sizeof(kDispatchPattern);

/* swapgs; mov gs:[10h], rsp; mov rsp, gs:[1A8h] */
const u8 kEntryPrefix[] = {0x0F, 0x01, 0xF8, 0x65, 0x48, 0x89, 0x24,
                           0x25, 0x10, 0x00, 0x00, 0x00, 0x65, 0x48,
                           0x8B, 0x24, 0x25, 0xA8, 0x01, 0x00, 0x00};
constexpr u32 kEntryPrefixLength = sizeof(kEntryPrefix);

constexpr u32 kEntryScanLimit = 0x800;
constexpr u32 kMinimumSpliceOffset = 0x40;

bool is_ntoskrnl_address(u64 address) {
    const driver_state& global = state();
    return global.kernel_base != 0 && address >= global.kernel_base &&
           address < global.kernel_base + global.kernel_size;
}

bool find_dispatch_splice(const u8* entry_bytes, u32 length,
                          u32* splice_offset) {
    if (entry_bytes == nullptr || splice_offset == nullptr ||
        length < kDispatchPatternLength) {
        return false;
    }
    u32 found = 0;
    u32 offset = 0;
    for (u32 index = 0; index + kDispatchPatternLength <= length; ++index) {
        if (RtlEqualMemory(entry_bytes + index, kDispatchPattern,
                           kDispatchPatternLength)) {
            if (found == 0) {
                offset = index + kDispatchPatternLength - 2; /* call rax */
            }
            ++found;
        }
    }
    if (found != 1 || offset < kMinimumSpliceOffset) {
        return false;
    }
    *splice_offset = offset;
    return true;
}

constexpr u32 kSharedDispatchScanLimit = 64u << 20;
/* KVA-shadow dispatch hook: implemented but disabled until the stub frame
 * offsets are reworked for the shadow stack layout; install stays
 * fail-closed on shadow entries. */
constexpr bool kShadowDispatchHook = false;

u64 g_probe_shared_dispatch = 0;
u32 g_probe_shared_count = 0;

ULONG_PTR flush_icache_callback(ULONG_PTR) {
    __wbinvd();
    int registers[4] = {};
    __cpuid(registers, 0);
    return 0;
}

void flush_instruction_caches() {
    KeIpiGenericCall(&flush_icache_callback, 0);
}

/* Finds "mov rax, r10; call rax" in the shared ntoskrnl text. On KVA-shadow
 * builds the LSTAR entry is the shadow stub, and the real dispatch lives in
 * the shared entry reached after the CR3 switch, so the fallback hooks that
 * call instruction instead of the MSR. */
bool find_shared_dispatch(u64 entry, u64* dispatch_va, u32* matches) {
    if (dispatch_va == nullptr || matches == nullptr) {
        return false;
    }
    *dispatch_va = 0;
    *matches = 0;
    u64 base = 0;
    u32 size = 0;
    if (!kernel_module_find("ntoskrnl.exe", &base, &size) || base == 0 ||
        size == 0 || size > kSharedDispatchScanLimit) {
        return false;
    }
    void* image = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, 'hksD');
    if (image == nullptr) {
        return false;
    }
    read_kernel_tolerant(base, image, size, nullptr);
    const u8* bytes = static_cast<const u8*>(image);
    u32 found = 0;
    u32 window = 0;
    u64 best = 0;
    u64 best_delta = ~0ULL;
    for (u32 index = 0; index + kDispatchPatternLength <= size; ++index) {
        if (RtlEqualMemory(bytes + index, kDispatchPattern,
                           kDispatchPatternLength)) {
            const u64 address = base + index;
            ++found;
            if (address >= entry && address < entry + 0x2000) {
                ++window;
                const u64 delta = address - entry;
                if (delta < best_delta) {
                    best_delta = delta;
                    best = address;
                }
            }
            index += kDispatchPatternLength - 1;
        }
    }
    ExFreePoolWithTag(image, 'hksD');
    if (window != 0) {
        *dispatch_va = best;
        *matches = window;
    } else {
        *matches = found;
    }
    return window != 0;
}

/* Executes the original service and hides debug-register state from the
 * returned CONTEXT. */
u64 run_context_query(const syscall_probe_context* context) {
    typedef u64 (*context_query_fn)(u64, u64, u64, u64);
    auto function = reinterpret_cast<context_query_fn>(
        static_cast<ULONG_PTR>(context->target));
    const u64 status =
        function(context->arg0, context->arg1, context->arg2, context->arg3);
    if (!NT_SUCCESS(static_cast<NTSTATUS>(status)) || context->arg1 == 0 ||
        !is_user_pointer(context->arg1)) {
        return status;
    }
    __try {
        auto* user_context =
            reinterpret_cast<PCONTEXT>(static_cast<ULONG_PTR>(context->arg1));
        if ((user_context->ContextFlags & CONTEXT_DEBUG_REGISTERS) != 0) {
            user_context->Dr0 = 0;
            user_context->Dr1 = 0;
            user_context->Dr2 = 0;
            user_context->Dr3 = 0;
            user_context->Dr6 = 0;
            user_context->Dr7 = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Leave the original status untouched; never fault the syscall. */
    }
    return status;
}

}  // namespace

extern "C" void drvlite_syscall_probe(drvlite::syscall_probe_context* context) {
    using u32 = drvlite::u32;
    using u64 = drvlite::u64;
    using drvlite::infinity_state;
    using drvlite::state;
    infinity_state& hook = state().infinity;
    if (!hook.installed || context == nullptr) {
        return;
    }
    InterlockedIncrement64(&hook.total_syscalls);
    const u32 syscall_number = context->syscall_number & 0xFFF;

    bool filtered = false;
    for (u32 index = 0; index < hook.filter_count; ++index) {
        if (hook.filter[index] == syscall_number) {
            filtered = true;
            break;
        }
    }
    if (filtered) {
        InterlockedIncrement64(&hook.filtered_hits);
    }

    /* syscall shield */
    if (hook.shield_enabled && hook.shield_ssn_count != 0) {
        const u32 pid = static_cast<u32>(
            reinterpret_cast<ULONG_PTR>(PsGetCurrentProcessId()));
        bool process_matches = hook.shield_pid == 0 || hook.shield_pid == pid;
        if (process_matches) {
            for (u32 index = 0; index < hook.shield_ssn_count; ++index) {
                if (hook.shield_ssns[index] == syscall_number) {
                    InterlockedIncrement64(&hook.blocked_calls);
                    context->handled = 1;
                    context->status = static_cast<u32>(STATUS_ACCESS_DENIED);
                    return;
                }
            }
        }
    }

    /* hardware-breakpoint detection bypass */
    if (hook.hide_enabled && hook.get_context_ssn != 0 &&
        syscall_number == hook.get_context_ssn) {
        PETHREAD thread = nullptr;
        if (NT_SUCCESS(ObReferenceObjectByHandle(
                reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(context->arg0)),
                0, *PsThreadType, UserMode, reinterpret_cast<PVOID*>(&thread),
                nullptr)) &&
            thread != nullptr) {
            PEPROCESS process = PsGetThreadProcess(thread);
            const u32 target_pid = static_cast<u32>(
                reinterpret_cast<ULONG_PTR>(PsGetProcessId(process)));
            const bool process_matches =
                hook.hide_pid == 0 || hook.hide_pid == target_pid;
            if (process_matches) {
                const drvlite::u64 status = run_context_query(context);
                InterlockedIncrement64(&hook.hidden_queries);
                context->handled = 1;
                context->status = static_cast<drvlite::u32>(status);
                ObDereferenceObject(thread);
                return;
            }
            ObDereferenceObject(thread);
        }
    }
}

}  // namespace drvlite

namespace drvlite {

bool infinity_installed() {
    return state().infinity.installed;
}

bool infinity_install(const u32* ssn_filter, u32 filter_count) {
    infinity_state& hook = state().infinity;
    if (hook.installed) {
        hook.error = DRVLITE_ERR_ALREADY;
        return false;
    }
    if (filter_count > DRVLITE_MAX_SSN_FILTER ||
        (filter_count != 0 && ssn_filter == nullptr)) {
        hook.error = DRVLITE_ERR_PARAM;
        return false;
    }
    if (state().kernel_base == 0 || !is_ntoskrnl_address(state().kernel_base)) {
        hook.error = DRVLITE_ERR_NOT_LOADED;
        return false;
    }
    hook = infinity_state{};

    const u32 processors = active_processor_count();
    if (processors == 0 || processors > kMaxProcessors) {
        hook.error = DRVLITE_ERR_INTERNAL;
        return false;
    }
    /* Per-CPU MSR reads are unreliable in some guests; the LSTAR value is
     * identical across processors, so read the current processor and restore
     * the same value everywhere. */
    const u64 entry = __readmsr(kLstarMsr);
    if (entry == 0 || !is_ntoskrnl_address(entry)) {
        hook.error = DRVLITE_ERR_UNSUPPORTED;
        return false;
    }

    u8 entry_bytes[kEntryScanLimit] = {};
    if (!read_kernel(entry, entry_bytes, sizeof(entry_bytes))) {
        hook.error = DRVLITE_ERR_IO;
        return false;
    }
    if (!RtlEqualMemory(entry_bytes, kEntryPrefix, kEntryPrefixLength)) {
        hook.error = DRVLITE_ERR_UNSUPPORTED;
        return false;
    }
    u32 splice_offset = 0;
    if (!find_dispatch_splice(entry_bytes, sizeof(entry_bytes),
                              &splice_offset)) {
        if (!kShadowDispatchHook) {
            hook.error = DRVLITE_ERR_UNSUPPORTED;
            return false;
        }
        /* KVA-shadow: the LSTAR target is the shadow stub, so the dispatch
         * pattern only exists in the shared entry. Hook the shared
         * `mov rax, r10; call rax` instruction instead. */
        u64 shared = 0;
        u32 matches = 0;
        u8 dispatch_bytes[8] = {};
        if (!find_shared_dispatch(entry, &shared, &matches) || matches == 0 ||
            !is_ntoskrnl_address(shared) ||
            !read_kernel(shared, dispatch_bytes, sizeof(dispatch_bytes)) ||
            !RtlEqualMemory(dispatch_bytes, kDispatchPattern,
                            kDispatchPatternLength)) {
            hook.error = DRVLITE_ERR_UNSUPPORTED;
            return false;
        }
        const u32 trampoline_length =
            kDispatchPatternLength + core::kAbsoluteJumpLength;
        void* trampoline = exec_alloc(trampoline_length);
        if (trampoline == nullptr) {
            hook.error = DRVLITE_ERR_NOMEM;
            return false;
        }
        auto* tramp = static_cast<u8*>(trampoline);
        RtlCopyMemory(tramp, kDispatchPattern, kDispatchPatternLength);
        if (!core::write_absolute_jump(tramp + kDispatchPatternLength,
                                       core::kAbsoluteJumpLength,
                                       shared + kDispatchPatternLength)) {
            exec_free(trampoline);
            hook.error = DRVLITE_ERR_INTERNAL;
            return false;
        }
        u8 patch[core::kAbsoluteJumpLength] = {};
        if (!core::write_absolute_jump(
                patch, sizeof(patch),
                reinterpret_cast<u64>(&drvlite_syscall_entry_stub))) {
            exec_free(trampoline);
            hook.error = DRVLITE_ERR_INTERNAL;
            return false;
        }
        if (!write_kernel(shared, patch, sizeof(patch))) {
            exec_free(trampoline);
            hook.error = DRVLITE_ERR_INTERNAL;
            return false;
        }
        u8 verify[sizeof(patch)] = {};
        if (!read_kernel(shared, verify, sizeof(verify)) ||
            RtlCompareMemory(verify, patch, sizeof(patch)) != sizeof(patch)) {
            write_kernel(shared, dispatch_bytes, kDispatchPatternLength);
            exec_free(trampoline);
            hook.error = DRVLITE_ERR_INTERNAL;
            return false;
        }
        flush_instruction_caches();
        g_drvlite_infinity_resume = reinterpret_cast<u64>(tramp);
        g_drvlite_infinity_skip =
            reinterpret_cast<u64>(tramp) + kDispatchPatternLength;
        for (u32 index = 0; index < kDispatchPatternLength; ++index) {
            hook.original_call[index] = dispatch_bytes[index];
        }
        hook.code_hook = true;
        hook.code_address = shared;
        hook.entry = entry;
        hook.trampoline = reinterpret_cast<u64>(trampoline);
        hook.dispatch = reinterpret_cast<u64>(&drvlite_syscall_entry_stub);
        hook.splice_offset = 0;
        hook.patch_length = kDispatchPatternLength;
        hook.processor_count = processors;
        hook.filter_count = filter_count;
        for (u32 index = 0; index < filter_count; ++index) {
            hook.filter[index] = ssn_filter[index] & 0xFFF;
        }
        hook.installed = true;
        hook.supported = true;
        return true;
    }

    const u32 trampoline_size = splice_offset + core::kAbsoluteJumpLength;
    void* trampoline = exec_alloc(trampoline_size);
    if (trampoline == nullptr) {
        hook.error = DRVLITE_ERR_NOMEM;
        return false;
    }
    RtlCopyMemory(trampoline, entry_bytes, splice_offset);
    if (!core::write_absolute_jump(
            static_cast<u8*>(trampoline) + splice_offset,
            core::kAbsoluteJumpLength,
            reinterpret_cast<u64>(&drvlite_syscall_entry_stub))) {
        exec_free(trampoline);
        hook.error = DRVLITE_ERR_INTERNAL;
        return false;
    }

    g_drvlite_infinity_resume = entry + splice_offset;
    g_drvlite_infinity_skip = entry + splice_offset + 2;

    for (u32 index = 0; index < processors; ++index) {
        hook.original_lstar[index] = entry;
    }
    hook.processor_count = processors;
    hook.entry = entry;
    hook.trampoline = reinterpret_cast<u64>(trampoline);
    hook.dispatch = reinterpret_cast<u64>(&drvlite_syscall_entry_stub);
    hook.splice_offset = splice_offset;
    hook.patch_length = splice_offset;

    if (!write_msr_all_cpus(kLstarMsr, reinterpret_cast<u64>(trampoline))) {
        exec_free(trampoline);
        hook = infinity_state{};
        hook.error = DRVLITE_ERR_INTERNAL;
        return false;
    }
    if (__readmsr(kLstarMsr) != reinterpret_cast<u64>(trampoline)) {
        write_msr_per_cpu(kLstarMsr, hook.original_lstar, processors);
        exec_free(trampoline);
        hook = infinity_state{};
        hook.error = DRVLITE_ERR_INTERNAL;
        return false;
    }

    hook.filter_count = filter_count;
    for (u32 index = 0; index < filter_count; ++index) {
        hook.filter[index] = ssn_filter[index] & 0xFFF;
    }
    hook.installed = true;
    hook.supported = true;
    return true;
}

bool infinity_remove() {
    infinity_state& hook = state().infinity;
    if (!hook.installed) {
        return true;
    }
    if (hook.code_hook) {
        if (!write_kernel(hook.code_address, hook.original_call,
                          kDispatchPatternLength)) {
            return false;
        }
        u8 verify_code[8] = {};
        if (!read_kernel(hook.code_address, verify_code,
                         kDispatchPatternLength) ||
            RtlCompareMemory(verify_code, hook.original_call,
                             kDispatchPatternLength) !=
                kDispatchPatternLength) {
            return false;
        }
        flush_instruction_caches();
        LARGE_INTEGER code_delay;
        code_delay.QuadPart = -10000LL * 20; /* 20 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &code_delay);
        void* code_trampoline = reinterpret_cast<void*>(hook.trampoline);
        hook.installed = false;
        hook.code_hook = false;
        hook.code_address = 0;
        hook.trampoline = 0;
        hook.entry = 0;
        exec_free(code_trampoline);
        return true;
    }
    if (!write_msr_per_cpu(kLstarMsr, hook.original_lstar,
                           hook.processor_count)) {
        return false;
    }
    if (__readmsr(kLstarMsr) != hook.entry) {
        return false;
    }
    /* Give an in-flight syscall on any processor a moment to leave the
     * trampoline before the pool block is released. */
    LARGE_INTEGER delay;
    delay.QuadPart = -10000LL * 20; /* 20 ms */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    void* trampoline = reinterpret_cast<void*>(hook.trampoline);
    hook.installed = false;
    hook.trampoline = 0;
    hook.entry = 0;
    exec_free(trampoline);
    return true;
}

void infinity_probe_fill(void* buffer, usize capacity) {
    if (buffer == nullptr || capacity < sizeof(infinity_probe_record)) {
        return;
    }
    infinity_probe_record record{};
    record.magic = kInfinityProbeMagic;
    record.kernel_base = state().kernel_base;

    u64 lstar[kMaxProcessors] = {};
    const u32 processors = active_processor_count();
    u32 executed = 0;
    u32 debug_processor = 0;
    u64 debug_value = 0;
    read_msr_all_cpus(kLstarMsr, lstar, processors, &executed, &debug_processor,
                      &debug_value);
    record.processor_count = processors;
    record.reserved = executed;
    record.p1_offset = debug_processor;
    record.p2_offset = static_cast<u32>(debug_value);
    record.p2_count = static_cast<u32>(debug_value >> 32);
    /* The all-CPU read is unreliable in some guests; probe the current
     * processor directly, which is also what the install path uses. */
    const u64 entry = __readmsr(kLstarMsr);
    record.lstar[7] = entry;
    record.lstar[0] = entry;
    if (entry == 0) {
        RtlCopyMemory(buffer, &record, sizeof(record));
        return;
    }
    bool all_equal = true;
    for (u32 index = 1; index < processors; ++index) {
        if (lstar[index] != entry) {
            all_equal = false;
            break;
        }
    }
    if (all_equal) {
        record.flags |= 0x1;
    }
    u8 entry_bytes[sizeof(record.entry_bytes)] = {};
    read_kernel_tolerant(entry, entry_bytes, sizeof(entry_bytes), nullptr);
    RtlCopyMemory(record.entry_bytes, entry_bytes, sizeof(record.entry_bytes));

    u32 prefix_match = 0;
    while (prefix_match < kEntryPrefixLength &&
           entry_bytes[prefix_match] == kEntryPrefix[prefix_match]) {
        ++prefix_match;
    }
    record.prefix_match = prefix_match;
    if (prefix_match == kEntryPrefixLength) {
        record.flags |= 0x2;
    }
    if (entry_bytes[0] == 0x0F && entry_bytes[1] == 0x01 &&
        entry_bytes[2] == 0xF8) {
        record.flags |= 0x4;
    }
    /* Pattern 1: mov rax, r10; call rax. */
    for (u32 index = 0; index + kDispatchPatternLength <= sizeof(entry_bytes);
         ++index) {
        if (RtlEqualMemory(entry_bytes + index, kDispatchPattern,
                           kDispatchPatternLength)) {
            if (record.p1_count == 0) {
                record.p1_offset = index;
            }
            ++record.p1_count;
        }
    }
    if (record.p1_count == 1) {
        record.flags |= 0x8;
    }
    /* Pattern 2: mov rax, r10; call r/m (any modrm). Records the encoding
     * used by builds where the call target form changed. */
    const u8 p2_prefix[] = {0x49, 0x8B, 0xC2, 0xFF};
    for (u32 index = 0; index + 6 <= sizeof(entry_bytes); ++index) {
        if (RtlEqualMemory(entry_bytes + index, p2_prefix, sizeof(p2_prefix))) {
            if (record.p2_count == 0) {
                record.p2_offset = index;
                record.p2_modrm = entry_bytes[index + 4];
            }
            ++record.p2_count;
        }
    }
    if (record.p2_count != 0) {
        record.flags |= 0x10;
    }
    /* KVA-shadow: the dispatch call lives in the shared entry, not in the
     * LSTAR stub. Report the shared-dispatch candidate for diagnostics. */
    u64 shared = 0;
    u32 matches = 0;
    if (find_shared_dispatch(entry, &shared, &matches)) {
        record.jmp_offset = matches;
        record.jmp_ok = 1;
        record.jmp_target = shared;
    }
    RtlCopyMemory(buffer, &record, sizeof(record));
}

void infinity_fill_status(drvlite_infinity_status_rsp* response) {
    if (response == nullptr) {
        return;
    }
    const infinity_state& hook = state().infinity;
    response->installed = hook.installed ? 1u : 0u;
    response->supported = hook.supported ? 1u : 0u;
    response->entry = hook.entry;
    response->dispatch = hook.dispatch;
    response->total_syscalls = static_cast<u64>(hook.total_syscalls);
    response->filtered_hits = static_cast<u64>(hook.filtered_hits);
    response->handled_calls = static_cast<u64>(hook.hidden_queries) +
                              static_cast<u64>(hook.blocked_calls);
    response->spliced_bytes = hook.patch_length;
    response->flags = hook.error;
}

bool infinity_set_hide(bool enable, u32 pid, u32 get_context_ssn) {
    infinity_state& hook = state().infinity;
    if (enable && get_context_ssn == 0) {
        return false;
    }
    hook.hide_enabled = enable;
    hook.hide_pid = pid;
    hook.get_context_ssn = enable ? (get_context_ssn & 0xFFF) : 0;
    return true;
}

bool infinity_set_shield(bool enable, u32 pid, const u32* ssns, u32 count) {
    infinity_state& hook = state().infinity;
    if (count > DRVLITE_MAX_SSN_FILTER || (count != 0 && ssns == nullptr)) {
        return false;
    }
    hook.shield_enabled = enable;
    hook.shield_pid = pid;
    hook.shield_ssn_count = enable ? count : 0;
    for (u32 index = 0; index < count && index < DRVLITE_MAX_SSN_FILTER;
         ++index) {
        hook.shield_ssns[index] = ssns[index] & 0xFFF;
    }
    return true;
}

}  // namespace drvlite
