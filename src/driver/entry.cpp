/*
 * blook-drv-lite - DriverEntry for a manually mapped image.
 *
 * There is no DriverObject, no unload routine and no registry path in the
 * production load model: the loader passes a pointer to a *kernel copy* of the
 * kchannel bootstrap in the DriverObject parameter slot. The image only
 * initialises its own state and the kchannel transport and then waits for the
 * user-mode SDK.
 */
#include "dse/dse.h"
#include "ipc/adapter.h"
#include "state.h"

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(registry_path);
    if (driver_object == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto* bootstrap =
        reinterpret_cast<const kc_bootstrap*>(driver_object);
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
    return STATUS_SUCCESS;
}
