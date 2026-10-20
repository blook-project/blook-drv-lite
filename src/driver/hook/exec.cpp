#include "exec.h"

namespace drvlite {

void* exec_alloc(usize size) {
    if (size == 0 || size > 0x10000) {
        return nullptr;
    }
    void* memory =
        ExAllocatePool2(POOL_FLAG_NON_PAGED_EXECUTE, size, kExecPoolTag);
    if (memory == nullptr) {
        return nullptr;
    }
    RtlZeroMemory(memory, size);
    return memory;
}

void exec_free(void* address) {
    if (address != nullptr) {
        ExFreePoolWithTag(address, static_cast<ULONG>(kExecPoolTag));
    }
}

}  // namespace drvlite
