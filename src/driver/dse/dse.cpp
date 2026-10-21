#include "dse.h"

#include "../mem/kva.h"
#include "../mem/modules.h"
#include "../state.h"
#include "../util.h"

namespace drvlite {
namespace {

constexpr u32 kValidatePatchLength = 3;

ULONG_PTR flush_icache_callback(ULONG_PTR context) {
    UNREFERENCED_PARAMETER(context);
    __wbinvd();
    int registers[4] = {};
    __cpuid(registers, 0);
    return 0;
}

void flush_instruction_caches() {
    KeIpiGenericCall(&flush_icache_callback, 0);
}

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
    bool used_legacy = false;
    if (resolved.code != core::dse_code::ok) {
        /* Windows 7 era kernels have g_CiEnabled in ntoskrnl. Supported
         * builds (Win10/11) fail here and report unsupported. */
        if (image != nullptr) {
            ExFreePoolWithTag(image, 'esdD');
            image = nullptr;
        }
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
                used_legacy = true;
            }
        }
    }
    /* Locate CiValidateImageHeader while the ci.dll image is still in pool:
     * patching it to return success makes the DSE disable functional even when
     * the g_CiOptions write alone is not honoured by the loader. */
    u64 validate_address = 0;
    if (!used_legacy && image != nullptr && ci_base != 0 && ci_size != 0) {
        core::pe_image parsed{};
        if (core::pe_parse(image, ci_size, ci_base, &parsed) ==
            core::pe_code::ok) {
            const u32 rva =
                core::pe_find_export_rva(parsed, "CiValidateImageHeader");
            if (rva != 0) {
                validate_address = ci_base + rva;
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
    if (validate_address != 0) {
        u8 code[sizeof(dse.original_code)] = {};
        if (read_kernel(validate_address, code, sizeof(code))) {
            dse.validate_address = validate_address;
            RtlCopyMemory(dse.original_code, code, sizeof(code));
        }
    }
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
    response->reserved = dse.via | (dse.code_patched != 0 ? 2u : 0u);
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
    if (observed != value) {
        return false;
    }
    /* Functional disable: make CiValidateImageHeader return success. */
    if (value == 0 && dse.validate_address != 0 && dse.code_patched == 0) {
        const u8 patch[kValidatePatchLength] = {0x33, 0xC0, 0xC3};
        if (!write_kernel(dse.validate_address, patch, sizeof(patch))) {
            return false;
        }
        u8 verify[kValidatePatchLength] = {};
        if (!read_kernel(dse.validate_address, verify, sizeof(verify)) ||
            RtlCompareMemory(verify, patch, sizeof(patch)) != sizeof(patch)) {
            return false;
        }
        flush_instruction_caches();
        dse.code_patched = 1;
    }
    return true;
}

bool dse_restore() {
    dse_state& dse = state().dse;
    if (!dse.supported) {
        return true;
    }
    bool ok = true;
    if (dse.code_patched != 0 && dse.validate_address != 0) {
        if (write_kernel(dse.validate_address, dse.original_code,
                         kValidatePatchLength)) {
            u8 verify[kValidatePatchLength] = {};
            if (read_kernel(dse.validate_address, verify, sizeof(verify)) &&
                RtlCompareMemory(verify, dse.original_code,
                                 kValidatePatchLength) ==
                    kValidatePatchLength) {
                dse.code_patched = 0;
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
        flush_instruction_caches();
    }
    if (!dse_set(dse.original_value)) {
        ok = false;
    }
    if (ok) {
        dse.patched = false;
    }
    return ok;
}

}  // namespace drvlite
