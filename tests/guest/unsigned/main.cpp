/*
 * drvlite-unsigned - deliberately unsigned test driver.
 *
 * Used by the DSE e2e case: loading it fails while Driver Signature
 * Enforcement is on and succeeds after DRVLITE_OP_DSE_SET. It does nothing
 * else on purpose.
 */
#include <ntddk.h>

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(driver_object);
    UNREFERENCED_PARAMETER(registry_path);
    DbgPrint("[drvlite] unsigned test driver loaded\n");
    return STATUS_SUCCESS;
}
