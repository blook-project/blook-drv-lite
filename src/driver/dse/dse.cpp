#include "dse.h"

#include "../mem/kva.h"
#include "../mem/modules.h"
#include "../state.h"
#include "../util.h"

namespace drvlite {
namespace {

core::dse_resolution resolve_from_module(const char* module_name, u64* base,
                                         u32* size, u8** image_buffer) {
    core::dse_resolution result;
    if (!kernel_module_find(module_name, base, size) || *base == 0 ||
        *size == 0 || *size > (64u << 20)) {
        result.code = core::dse_code::bad_image;
        return result;
    }
    void* image = ExAllocatePool2(POOL_FLAG_NON_PAGED, *size, 'esdD');
    if (image == nullptr) {
        result.code = core::dse_code::bad_image;
        return result;
    }
    core::read_result read_result;
    read_kernel_tolerant(*base, image, *size, &read_result);
    result = core::dse_resolve_ci_options(static_cast<const u8*>(image), *size,
                                          *base);
    *image_buffer = static_cast<u8*>(image);
    return result;
}

u32 read_options_value(u64 address) {
    u32 value = 0;
    if (!read_kernel(address, &value, sizeof(value))) {
        return 0;
    }
    return value;
}

}  // namespace

bool dse_initialize() {
    dse_state& dse = state().dse;
    dse = dse_state{};
    u64 ci_base = 0;
    u32 ci_size = 0;
    u8* image = nullptr;
    core::dse_resolution resolved =
        resolve_from_module("ci.dll", &ci_base, &ci_size, &image);
    u32 via = resolved.via;
    if (resolved.code != core::dse_code::ok) {
        /* Windows 7 era kernels have g_CiEnabled in ntoskrnl. Supported
         * builds (Win10/11) fail here and report unsupported. */
        image = nullptr;
        u64 nt_base = 0;
        u32 nt_size = 0;
        core::dse_resolution legacy =
            resolve_from_module("ntoskrnl.exe", &nt_base, &nt_size, &image);
        (void)legacy;
        if (image != nullptr) {
            legacy =
                core::dse_resolve_legacy_ci_enabled(image, nt_size, nt_base);
            if (legacy.code == core::dse_code::ok) {
                resolved = legacy;
                ci_base = nt_base;
                ci_size = nt_size;
                via = legacy.via;
            }
        }
    }
    if (image != nullptr) {
        ExFreePoolWithTag(image, 'esdD');
    }
    if (resolved.code != core::dse_code::ok) {
        dse.error = static_cast<u32>(resolved.code);
        dse.supported = false;
        return false;
    }
    dse.address = resolved.options_va;
    dse.ci_base = ci_base;
    dse.ci_size = ci_size;
    dse.reference_rva = resolved.reference_rva;
    dse.via = via;
    dse.supported = true;
    dse.original_value = read_options_value(dse.address);
    dse.current_value = dse.original_value;
    dse.patched = false;
    return true;
}

bool dse_available() {
    return state().dse.supported;
}

void dse_fill_status(drvlite_dse_rsp* response) {
    if (response == nullptr) {
        return;
    }
    const dse_state& dse = state().dse;
    response->supported = dse.supported ? 1u : 0u;
    response->patched = dse.patched ? 1u : 0u;
    response->current = dse.current_value;
    response->original = dse.original_value;
    response->nt_build = state().nt_build;
    response->flags = dse.error;
    response->address = dse.address;
    response->ci_base = dse.ci_base;
    response->ci_size = dse.ci_size;
    response->reserved = dse.via;
}

bool dse_set(u32 value) {
    dse_state& dse = state().dse;
    if (!dse.supported || dse.address == 0) {
        return false;
    }
    if (!write_kernel(dse.address, &value, sizeof(value))) {
        return false;
    }
    /* Verify the write and report the observed value. */
    const u32 observed = read_options_value(dse.address);
    dse.current_value = observed;
    dse.patched = observed != dse.original_value;
    return observed == value;
}

bool dse_restore() {
    dse_state& dse = state().dse;
    if (!dse.supported) {
        return true;
    }
    const bool ok = dse_set(dse.original_value);
    if (ok) {
        dse.patched = false;
    }
    return ok;
}

}  // namespace drvlite
