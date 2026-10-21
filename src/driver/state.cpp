#include "state.h"

#include "dse/dse.h"
#include "hook/infinity.h"
#include "ipc/adapter.h"
#include "mem/modules.h"
#include "util.h"

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                PUNICODE_STRING registry_path);

namespace drvlite {
namespace {

driver_state g_state;

}  // namespace

driver_state& state() {
    return g_state;
}

NTSTATUS state_initialize(const kc_bootstrap* bootstrap) {
    driver_state& global = g_state;
    RtlZeroMemory(&global, sizeof(global));
    KeInitializeSpinLock(&global.patch_lock);
    global.bootstrap = bootstrap;
    global.nt_build = nt_build_number();
    global.driver_base =
        image_base_from_pointer(reinterpret_cast<const void*>(&DriverEntry));
    global.driver_size =
        global.driver_base != 0 ? image_size_at(global.driver_base) : 0;
    u64 kernel_base = 0;
    u32 kernel_size = 0;
    if (kernel_module_find("ntoskrnl.exe", &kernel_base, &kernel_size)) {
        global.kernel_base = kernel_base;
        global.kernel_size = kernel_size;
    }
    global.features = DRVLITE_FEATURE_PHYS_READ | DRVLITE_FEATURE_PHYS_WRITE |
                      DRVLITE_FEATURE_KERNEL_MEMORY |
                      DRVLITE_FEATURE_PROCESS_MEMORY | DRVLITE_FEATURE_MODULES |
                      DRVLITE_FEATURE_HANDLES | DRVLITE_FEATURE_INFINITY_HOOK |
                      DRVLITE_FEATURE_HWBP_HIDE |
                      DRVLITE_FEATURE_SYSCALL_SHIELD;
    global.client_pid = bootstrap != nullptr && bootstrap->peer_pid != 0
                            ? bootstrap->peer_pid
                            : static_cast<u32>(reinterpret_cast<ULONG_PTR>(
                                  PsGetCurrentProcessId()));
    PEPROCESS client = nullptr;
    if (NT_SUCCESS(PsLookupProcessByProcessId(ULongToHandle(global.client_pid),
                                              &client)) &&
        client != nullptr) {
        global.client_process = client;
    }
    /* Diagnostic page for the physical read/write e2e cases. */
    void* test_page = ExAllocatePool2(POOL_FLAG_NON_PAGED, 0x1000, 'pdlD');
    if (test_page != nullptr) {
        RtlFillMemory(test_page, 0x1000, 0xCC);
        RtlCopyMemory(test_page, "DRVLITE-PHYS", 12);
        global.test_page = test_page;
        const PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(test_page);
        global.test_page_pa = static_cast<u64>(physical.QuadPart) & ~0xFFFULL;
    }
    return STATUS_SUCCESS;
}

void cleanup_all() {
    driver_state& global = g_state;
    if (global.cleanup_done) {
        return;
    }
    global.cleanup_done = true;
    global.shutting_down = true;
    /* Reverse order of installation: hooks first, then DSE. */
    infinity_remove();
    if (global.dse.patched) {
        dse_restore();
    }
    if (global.client_process != nullptr) {
        ObDereferenceObject(global.client_process);
        global.client_process = nullptr;
    }
    if (global.test_page != nullptr) {
        ExFreePoolWithTag(global.test_page, 'pdlD');
        global.test_page = nullptr;
    }
    global.handles_open = 0;
}

void state_destroy() {
    cleanup_all();
    adapter_stop();
    RtlZeroMemory(&g_state, sizeof(g_state));
}

}  // namespace drvlite
