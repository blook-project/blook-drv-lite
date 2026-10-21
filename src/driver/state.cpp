#include "state.h"

#include "dse/dse.h"
#include "hook/infinity.h"
#include "ipc/adapter.h"
#include "mem/modules.h"
#include "util.h"

#include <ntimage.h>

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                PUNICODE_STRING registry_path);

extern "C" PVOID NTAPI RtlPcToFileHeader(PVOID pc_value, PVOID* base_of_image);
extern "C" PIMAGE_NT_HEADERS NTAPI RtlImageNtHeader(PVOID base);

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
    /* Resolve the image extent from the loader's module list entry instead of
     * faulting pages of our own image during DriverEntry (that can recurse
     * into the image section lock and deadlock under the SCM load). */
    PVOID image_base = nullptr;
    if (RtlPcToFileHeader(reinterpret_cast<PVOID>(&DriverEntry), &image_base) &&
        image_base != nullptr) {
        global.driver_base = reinterpret_cast<u64>(image_base);
        __try {
            const auto* headers = RtlImageNtHeader(image_base);
            if (headers != nullptr) {
                global.driver_size = headers->OptionalHeader.SizeOfImage;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            global.driver_size = 0;
        }
    }
    /* Same trick for ntoskrnl: resolve a known export, not a list walk. */
    UNICODE_STRING routine_name;
    RtlInitUnicodeString(&routine_name, L"PsLookupProcessByProcessId");
    PVOID routine = MmGetSystemRoutineAddress(&routine_name);
    PVOID kernel_image = nullptr;
    if (routine != nullptr && RtlPcToFileHeader(routine, &kernel_image) &&
        kernel_image != nullptr) {
        global.kernel_base = reinterpret_cast<u64>(kernel_image);
        __try {
            const auto* headers = RtlImageNtHeader(kernel_image);
            if (headers != nullptr) {
                global.kernel_size = headers->OptionalHeader.SizeOfImage;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            global.kernel_size = 0;
        }
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
    return STATUS_SUCCESS;
}

/* Process lookup and the diagnostic page are deferred to a PASSIVE system
 * thread after DriverEntry returns: they are not needed to load and doing
 * them while the service manager holds the load lock is unnecessary risk. */
NTSTATUS state_deferred_initialize() {
    driver_state& global = g_state;
    if (global.client_process == nullptr && global.client_pid != 0) {
        PEPROCESS client = nullptr;
        if (NT_SUCCESS(PsLookupProcessByProcessId(
                ULongToHandle(global.client_pid), &client)) &&
            client != nullptr) {
            global.client_process = client;
        }
    }
    if (global.test_page == nullptr) {
        void* test_page = ExAllocatePool2(POOL_FLAG_NON_PAGED, 0x1000, 'pdlD');
        if (test_page != nullptr) {
            RtlFillMemory(test_page, 0x1000, 0xCC);
            RtlCopyMemory(test_page, "DRVLITE-PHYS", 12);
            global.test_page = test_page;
            const PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(test_page);
            global.test_page_pa =
                static_cast<u64>(physical.QuadPart) & ~0xFFFULL;
        }
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
