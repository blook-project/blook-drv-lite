/*
 * blook-drv-lite - DriverEntry.
 *
 * Two load models are supported, both defined by the kchannel ABI:
 *
 *  - manual map (production): the loader passes a pointer to a *kernel copy*
 *    of the kc_bootstrap in the DriverObject parameter slot. There is no
 *    DriverObject, no device and no registry path in this model.
 *  - service load (dev/debug, used by the e2e matrix): the SCM calls
 *    DriverEntry with a real DriverObject and the service registry path; the
 *    bootstrap blob is read from the service's Parameters\Bootstrap value.
 *    kchannel documents this mode for testing and it behaves identically.
 *
 * Detection is by magic: a bootstrap starts with KC_BOOTSTRAP_MAGIC, a
 * PDRIVER_OBJECT starts with IO_TYPE_DRIVER.
 *
 * For service load an optional Parameters\Stage DWORD limits how far the
 * initialisation goes (0 = nothing, 1 = state, 2 = +adapter, 3 = +DSE,
 * 4 = +worker). The driver records the last completed step in
 * Parameters\StageDone, which makes a hang in a specific step diagnosable.
 */
#include "dse/dse.h"
#include "ipc/adapter.h"
#include "state.h"

namespace {

constexpr ULONG kBootstrapValueMax = sizeof(kc_bootstrap) + 64;
kc_bootstrap g_bootstrap_copy{};

NTSTATUS open_parameters_key(PUNICODE_STRING service_path, HANDLE* key) {
    *key = nullptr;
    wchar_t buffer[512] = {};
    UNICODE_STRING full{};
    full.Buffer = buffer;
    full.MaximumLength = static_cast<USHORT>(sizeof(buffer) - sizeof(wchar_t));
    if (service_path->Length > full.MaximumLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyUnicodeString(&full, service_path);
    if (!NT_SUCCESS(RtlAppendUnicodeToString(&full, L"\\Parameters"))) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &full,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    return ZwOpenKey(key, KEY_READ | KEY_SET_VALUE, &attributes);
}

NTSTATUS read_registry_dword(PUNICODE_STRING service_path, PCWSTR name,
                             ULONG* out) {
    HANDLE key = nullptr;
    NTSTATUS status = open_parameters_key(service_path, &key);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    UNICODE_STRING value_name;
    RtlInitUnicodeString(&value_name, name);
    UCHAR value[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = {};
    ULONG length = 0;
    status = ZwQueryValueKey(key, &value_name, KeyValuePartialInformation,
                             value, sizeof(value), &length);
    ZwClose(key);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const auto* info =
        reinterpret_cast<const KEY_VALUE_PARTIAL_INFORMATION*>(value);
    if (info->Type != REG_DWORD || info->DataLength < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(out, info->Data, sizeof(ULONG));
    return STATUS_SUCCESS;
}

void write_registry_dword(PUNICODE_STRING service_path, PCWSTR name,
                          ULONG value) {
    HANDLE key = nullptr;
    if (!NT_SUCCESS(open_parameters_key(service_path, &key))) {
        return;
    }
    UNICODE_STRING value_name;
    RtlInitUnicodeString(&value_name, name);
    ZwSetValueKey(key, &value_name, 0, REG_DWORD, &value, sizeof(value));
    ZwClose(key);
}

NTSTATUS read_registry_bootstrap(PUNICODE_STRING service_path,
                                 kc_bootstrap* out) {
    if (service_path == nullptr || out == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    HANDLE key = nullptr;
    NTSTATUS status = open_parameters_key(service_path, &key);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    UNICODE_STRING value_name;
    RtlInitUnicodeString(&value_name, L"Bootstrap");
    UCHAR value[kBootstrapValueMax] = {};
    ULONG length = 0;
    status = ZwQueryValueKey(key, &value_name, KeyValuePartialInformation,
                             value, sizeof(value), &length);
    ZwClose(key);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const auto* info =
        reinterpret_cast<const KEY_VALUE_PARTIAL_INFORMATION*>(value);
    if (info->Type != REG_BINARY || info->DataLength < sizeof(kc_bootstrap)) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(out, info->Data, sizeof(kc_bootstrap));
    if (out->magic != KC_BOOTSTRAP_MAGIC ||
        out->struct_size != sizeof(kc_bootstrap)) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

void service_unload(PDRIVER_OBJECT driver_object) {
    UNREFERENCED_PARAMETER(driver_object);
    drvlite::cleanup_all();
    drvlite::state_destroy();
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                PUNICODE_STRING registry_path) {
    if (driver_object == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto* candidate =
        reinterpret_cast<const kc_bootstrap*>(driver_object);
    const bool manual_map = candidate->magic == KC_BOOTSTRAP_MAGIC &&
                            candidate->struct_size == sizeof(kc_bootstrap);
    const kc_bootstrap* bootstrap = nullptr;
    ULONG stage = 4;
    if (manual_map) {
        bootstrap = candidate;
    } else {
        ULONG configured = 4;
        if (NT_SUCCESS(
                read_registry_dword(registry_path, L"Stage", &configured)) &&
            configured <= 4) {
            stage = configured;
        }
        /* Stage 0 is a pure load test and needs no bootstrap. */
        if (stage >= 1) {
            const NTSTATUS read_status =
                read_registry_bootstrap(registry_path, &g_bootstrap_copy);
            if (!NT_SUCCESS(read_status)) {
                return read_status;
            }
            bootstrap = &g_bootstrap_copy;
        }
    }

    NTSTATUS status = STATUS_SUCCESS;
    /* Stage 1: per-image state (diagnostic page, module bases). */
    if (stage >= 1) {
        status = drvlite::state_initialize(bootstrap);
        if (!NT_SUCCESS(status)) {
            drvlite::state_destroy();
            return status;
        }
        if (!manual_map) {
            write_registry_dword(registry_path, L"StageDone", 1);
        }
    }
    /* Stage 2: kchannel kernel instance. */
    if (stage >= 2) {
        if (!drvlite::adapter_create(bootstrap)) {
            drvlite::state_destroy();
            return STATUS_UNSUCCESSFUL;
        }
        if (!manual_map) {
            write_registry_dword(registry_path, L"StageDone", 2);
        }
    }
    /* Stage 3: resolve ci.dll before the worker can accept requests. */
    if (stage >= 3) {
        if (drvlite::dse_initialize()) {
            drvlite::state().features |= DRVLITE_FEATURE_DSE;
        }
        if (!manual_map) {
            write_registry_dword(registry_path, L"StageDone", 3);
        }
    }
    /* Stage 4: start the PASSIVE-level worker. */
    if (stage >= 4) {
        if (!drvlite::adapter_start_worker()) {
            drvlite::state_destroy();
            return STATUS_UNSUCCESSFUL;
        }
        if (!manual_map) {
            write_registry_dword(registry_path, L"StageDone", 4);
        }
    }
    if (!manual_map) {
        driver_object->DriverUnload = &service_unload;
    }
    return STATUS_SUCCESS;
}
