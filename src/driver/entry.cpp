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
 */
#include "dse/dse.h"
#include "ipc/adapter.h"
#include "state.h"

namespace {

constexpr ULONG kBootstrapValueMax = sizeof(kc_bootstrap) + 64;
kc_bootstrap g_bootstrap_copy{};

NTSTATUS read_registry_bootstrap(PUNICODE_STRING service_path,
                                 kc_bootstrap* out) {
    if (service_path == nullptr || out == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
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
    HANDLE key = nullptr;
    NTSTATUS status = ZwOpenKey(&key, KEY_READ, &attributes);
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
    if (manual_map) {
        bootstrap = candidate;
    } else {
        const NTSTATUS read_status =
            read_registry_bootstrap(registry_path, &g_bootstrap_copy);
        if (!NT_SUCCESS(read_status)) {
            return read_status;
        }
        bootstrap = &g_bootstrap_copy;
    }
    NTSTATUS status = drvlite::state_initialize(bootstrap);
    if (!NT_SUCCESS(status)) {
        drvlite::state_destroy();
        return status;
    }
    if (!drvlite::adapter_create(bootstrap)) {
        drvlite::state_destroy();
        return STATUS_UNSUCCESSFUL;
    }
    /* Resolve ci.dll before the worker can accept requests. */
    if (drvlite::dse_initialize()) {
        drvlite::state().features |= DRVLITE_FEATURE_DSE;
    }
    if (!drvlite::adapter_start_worker()) {
        drvlite::state_destroy();
        return STATUS_UNSUCCESSFUL;
    }
    if (!manual_map) {
        driver_object->DriverUnload = &service_unload;
    }
    return STATUS_SUCCESS;
}
