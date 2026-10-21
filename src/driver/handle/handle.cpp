#include "handle.h"

#include "../mem/kva.h"
#include "../state.h"
#include "../util.h"

namespace drvlite {
namespace {

constexpr u32 kGrantedAccessBitCount = 25;
constexpr u32 kProcessProbeAccess = 0x1400; /* QUERY_INFORMATION | LIMITED */
constexpr u32 kThreadProbeAccess = 0x0840;  /* QUERY_INFORMATION | LIMITED */
constexpr u32 kMaxHandles = 1u << 22;
constexpr u32 kHandleEntrySize = core::kHandleEntrySize;

bool table_read_callback(void* context, u64 address, void* buffer, usize size) {
    UNREFERENCED_PARAMETER(context);
    return read_kernel(address, buffer, size);
}

core::table_reader kernel_table_reader() {
    core::table_reader reader;
    reader.ctx = nullptr;
    reader.read = &table_read_callback;
    return reader;
}

/*
 * Finds EPROCESS.ObjectTable by probing candidate pointer slots of the client
 * process: the first slot whose handle table resolves the freshly opened
 * handle to an entry carrying exactly our probe access is the right one.
 */
bool find_handle_entry(PEPROCESS client, u64 handle_value, u32 probe_access,
                       u64* entry_address, u8* entry_bytes) {
    if (client == nullptr || entry_address == nullptr ||
        entry_bytes == nullptr) {
        return false;
    }
    const u64 client_va = reinterpret_cast<u64>(client);
    const core::table_reader reader = kernel_table_reader();
    u32 expected_handle = static_cast<u32>(handle_value);
    for (u32 offset = 0x200; offset <= 0x900; offset += 8) {
        u64 table_pointer = 0;
        if (!read_kernel(client_va + offset, &table_pointer,
                         sizeof(table_pointer))) {
            continue;
        }
        if (!is_kernel_pointer(table_pointer) || (table_pointer & 0x7) != 0) {
            continue;
        }
        u64 table_code = 0;
        if (!read_kernel(table_pointer, &table_code, sizeof(table_code))) {
            continue;
        }
        const u32 level = static_cast<u32>(table_code & 0x3);
        if (level > 2) {
            continue;
        }
        u64 entry = 0;
        if (core::locate_entry(reader, table_code, expected_handle, kMaxHandles,
                               &entry) != core::table_code::ok) {
            continue;
        }
        u8 bytes[kHandleEntrySize] = {};
        if (!read_kernel(entry, bytes, sizeof(bytes))) {
            continue;
        }
        u32 bit_offset = 0;
        if (!core::find_access_field(bytes, sizeof(bytes),
                                     kGrantedAccessBitCount, probe_access,
                                     &bit_offset)) {
            continue;
        }
        *entry_address = entry;
        RtlCopyMemory(entry_bytes, bytes, sizeof(bytes));
        return true;
    }
    return false;
}

bool patch_access(PEPROCESS client, u64 handle_value, u32 probe_access,
                  u32 desired_access, u32* observed_out) {
    u64 entry = 0;
    u8 bytes[kHandleEntrySize] = {};
    if (!find_handle_entry(client, handle_value, probe_access, &entry, bytes)) {
        return false;
    }
    u32 bit_offset = 0;
    if (!core::find_access_field(bytes, sizeof(bytes), kGrantedAccessBitCount,
                                 probe_access, &bit_offset)) {
        return false;
    }
    core::set_bits(bytes, sizeof(bytes), bit_offset, kGrantedAccessBitCount,
                   desired_access);
    if (!write_kernel(entry, bytes, sizeof(bytes))) {
        return false;
    }
    /* Verify through a second read. */
    u8 verification[kHandleEntrySize] = {};
    if (!read_kernel(entry, verification, sizeof(verification))) {
        return false;
    }
    const u32 observed = core::get_bits(verification, sizeof(verification),
                                        bit_offset, kGrantedAccessBitCount);
    if (observed_out != nullptr) {
        *observed_out = observed;
    }
    return observed == desired_access;
}

}  // namespace

bool handle_open(u32 pid, u32 type, u32 tid, u32 access, handle_result* out) {
    if (out == nullptr) {
        return false;
    }
    *out = handle_result{};
    driver_state& global = state();
    if (global.client_process == nullptr) {
        return false;
    }
    const bool is_thread = type == DRVLITE_HANDLE_TYPE_THREAD;
    const u32 probe_access =
        is_thread ? kThreadProbeAccess : kProcessProbeAccess;
    const u32 desired_access = access;

    PVOID object = nullptr;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    if (is_thread) {
        PETHREAD thread = nullptr;
        if (tid == 0) {
            return false;
        }
        status = PsLookupThreadByThreadId(ULongToHandle(tid), &thread);
        if (!NT_SUCCESS(status) || thread == nullptr) {
            return false;
        }
        if (pid != 0 &&
            PsGetProcessId(PsGetThreadProcess(thread)) != ULongToHandle(pid)) {
            ObDereferenceObject(thread);
            return false;
        }
        object = thread;
    } else {
        PEPROCESS process = nullptr;
        status = PsLookupProcessByProcessId(ULongToHandle(pid), &process);
        if (!NT_SUCCESS(status) || process == nullptr) {
            return false;
        }
        object = process;
    }

    /* Preferred: open with the desired access directly from kernel mode. The
     * access check is bypassed for KernelMode callers, so the handle already
     * carries full permissions and no handle-table patching (or offset
     * discovery) is required. */
    HANDLE direct = nullptr;
    KAPC_STATE direct_apc{};
    KeStackAttachProcess(global.client_process, &direct_apc);
    status = ObOpenObjectByPointer(object, 0, nullptr, desired_access,
                                   is_thread ? *PsThreadType : *PsProcessType,
                                   KernelMode, &direct);
    KeUnstackDetachProcess(&direct_apc);
    if (NT_SUCCESS(status) && direct != nullptr &&
        direct != reinterpret_cast<HANDLE>(-1)) {
        out->handle = reinterpret_cast<u64>(direct);
        out->object = reinterpret_cast<u64>(object);
        out->requested = desired_access;
        out->granted = desired_access;
        global.handles_open += 1;
        ObDereferenceObject(object);
        return true;
    }

    HANDLE opened = nullptr;
    KAPC_STATE apc{};
    KeStackAttachProcess(global.client_process, &apc);
    status = ObOpenObjectByPointer(object, 0, nullptr, probe_access,
                                   is_thread ? *PsThreadType : *PsProcessType,
                                   KernelMode, &opened);
    KeUnstackDetachProcess(&apc);

    if (!NT_SUCCESS(status) || opened == nullptr ||
        opened == reinterpret_cast<HANDLE>(-1)) {
        ObDereferenceObject(object);
        return false;
    }

    u32 observed = 0;
    const bool patched =
        patch_access(global.client_process, reinterpret_cast<u64>(opened),
                     probe_access, desired_access, &observed);
    out->handle = reinterpret_cast<u64>(opened);
    out->object = reinterpret_cast<u64>(object);
    out->requested = desired_access;
    out->granted = patched ? observed : probe_access;
    global.handles_open += 1;

    /* The handle holds its own reference; release the lookup reference. */
    ObDereferenceObject(object);
    if (!patched) {
        /* Do not leak a handle whose access could not be rewritten. */
        KAPC_STATE close_apc{};
        KeStackAttachProcess(global.client_process, &close_apc);
        ZwClose(opened);
        KeUnstackDetachProcess(&close_apc);
        global.handles_open -= 1;
        out->handle = 0;
        return false;
    }
    return true;
}

bool handle_close(u32 pid, u64 handle) {
    UNREFERENCED_PARAMETER(pid);
    driver_state& global = state();
    if (handle == 0 || handle == static_cast<u64>(-1) ||
        global.client_process == nullptr) {
        return false;
    }
    KAPC_STATE apc{};
    KeStackAttachProcess(global.client_process, &apc);
    const NTSTATUS status = ZwClose(reinterpret_cast<HANDLE>(handle));
    KeUnstackDetachProcess(&apc);
    if (NT_SUCCESS(status) && global.handles_open > 0) {
        global.handles_open -= 1;
    }
    return NT_SUCCESS(status);
}

}  // namespace drvlite
