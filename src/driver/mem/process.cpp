#include "process.h"
#include "../util.h"

#include "kva.h"
#include "phys.h"

namespace drvlite {
namespace {

u64 read_cr3_from_eprocess(PEPROCESS process) {
    u64 value = 0;
    if (!read_kernel(
            reinterpret_cast<u64>(process) + kEprocessDirectoryTableBase,
            &value, sizeof(value))) {
        return 0;
    }
    return (value & ~0xFFFULL) != 0 ? value & ~0xFFFULL : 0;
}

u64 read_cr3_attached(PEPROCESS process) {
    u64 value = 0;
    KAPC_STATE apc{};
    KeStackAttachProcess(process, &apc);
    __try {
        value = __readcr3() & ~0xFFFULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
    }
    KeUnstackDetachProcess(&apc);
    return value;
}

}  // namespace

bool process_open(u32 pid, process_ref* out) {
    if (out == nullptr || pid == 0) {
        return false;
    }
    *out = process_ref{};
    PEPROCESS process = nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId(ULongToHandle(pid), &process)) ||
        process == nullptr) {
        return false;
    }
    out->process = process;
    out->pid = pid;
    return true;
}

void process_close(process_ref* out) {
    if (out != nullptr && out->process != nullptr) {
        ObDereferenceObject(out->process);
        out->process = nullptr;
    }
}

bool process_resolve(process_ref* process) {
    if (process == nullptr || process->process == nullptr) {
        return false;
    }
    process->peb = reinterpret_cast<u64>(PsGetProcessPeb(process->process));
    process->image_base =
        reinterpret_cast<u64>(PsGetProcessSectionBaseAddress(process->process));
    process->image_size = image_size_at(process->image_base);
    if (!is_user_pointer(process->peb) ||
        !is_user_pointer(process->image_base)) {
        return false;
    }
    const core::phys_reader reader = core_phys_reader();
    /* Candidate A: the value stored in KPROCESS.DirectoryTableBase. */
    u64 cr3 = read_cr3_from_eprocess(process->process);
    if (cr3 != 0 &&
        core::validate_cr3(reader, cr3, process->peb, process->image_base)) {
        process->cr3 = cr3;
        process->cr3_validated = true;
        return true;
    }
    /* Candidate B: what the CPU actually loads when attaching. Some
     * anti-cheat / protected processes keep a decoy in the process object. */
    cr3 = read_cr3_attached(process->process);
    if (cr3 != 0 &&
        core::validate_cr3(reader, cr3, process->peb, process->image_base)) {
        process->cr3 = cr3;
        process->cr3_validated = true;
        return true;
    }
    process->cr3 = 0;
    process->cr3_validated = false;
    return false;
}

bool process_read(process_ref* process, u64 virtual_address, void* buffer,
                  usize size, bool tolerant, core::read_result* result) {
    if (result != nullptr) {
        *result = core::read_result{};
    }
    if (process == nullptr || buffer == nullptr || size == 0) {
        return false;
    }
    if (!is_user_pointer(virtual_address) ||
        !is_user_pointer(virtual_address + size - 1)) {
        if (result != nullptr) {
            result->error = core::walk_code::non_canonical;
        }
        return false;
    }
    if (!process_resolve(process)) {
        if (result != nullptr) {
            result->error = core::walk_code::not_present;
        }
        return false;
    }
    const core::read_result value =
        core::read_virtual(core_phys_reader(), process->cr3, virtual_address,
                           buffer, size, tolerant);
    if (result != nullptr) {
        *result = value;
    }
    if (tolerant) {
        return value.copied != 0 || value.zero_filled == size;
    }
    return value.copied == size;
}

bool process_write(process_ref* process, u64 virtual_address,
                   const void* buffer, usize size) {
    if (process == nullptr || buffer == nullptr || size == 0) {
        return false;
    }
    if (!is_user_pointer(virtual_address) ||
        !is_user_pointer(virtual_address + size - 1)) {
        return false;
    }
    if (!process_resolve(process)) {
        return false;
    }
    const core::phys_reader reader = core_phys_reader();
    const u8* source = static_cast<const u8*>(buffer);
    usize done = 0;
    while (done < size) {
        core::translation mapped;
        if (core::translate(reader, process->cr3, virtual_address + done,
                            &mapped) != core::walk_code::ok) {
            return false;
        }
        const u64 offset_in_page =
            (virtual_address + done) & (static_cast<u64>(mapped.span) - 1);
        const u64 remaining_in_page = mapped.span - offset_in_page;
        const usize step = static_cast<usize>(remaining_in_page < (size - done)
                                                  ? remaining_in_page
                                                  : (size - done));
        if (!phys_write(mapped.physical, source + done, step)) {
            return false;
        }
        done += step;
    }
    return true;
}

}  // namespace drvlite
