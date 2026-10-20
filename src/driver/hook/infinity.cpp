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

    u64 lstar[kMaxProcessors] = {};
    const u32 processors = active_processor_count();
    if (processors == 0 || processors > kMaxProcessors) {
        hook.error = DRVLITE_ERR_INTERNAL;
        return false;
    }
    if (!read_msr_all_cpus(kLstarMsr, lstar, processors)) {
        hook.error = DRVLITE_ERR_INTERNAL;
        return false;
    }
    const u64 entry = lstar[0];
    if (entry == 0 || !is_ntoskrnl_address(entry)) {
        /* KVA shadow (or an unexpected entry) - fail cleanly. */
        hook.error = DRVLITE_ERR_UNSUPPORTED;
        return false;
    }
    for (u32 index = 1; index < processors; ++index) {
        if (lstar[index] != entry) {
            hook.error = DRVLITE_ERR_UNSUPPORTED;
            return false;
        }
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
        hook.error = DRVLITE_ERR_UNSUPPORTED;
        return false;
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
        hook.original_lstar[index] = lstar[index];
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
    u64 verify[kMaxProcessors] = {};
    if (!read_msr_all_cpus(kLstarMsr, verify, processors) ||
        verify[0] != reinterpret_cast<u64>(trampoline)) {
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
    if (!write_msr_per_cpu(kLstarMsr, hook.original_lstar,
                           hook.processor_count)) {
        return false;
    }
    u64 verify[kMaxProcessors] = {};
    read_msr_all_cpus(kLstarMsr, verify, hook.processor_count);
    if (verify[0] != hook.entry) {
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
