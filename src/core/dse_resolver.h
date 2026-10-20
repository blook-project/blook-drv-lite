/*
 * blook-drv-lite - resolves ci.dll's DSE options variable from a loaded
 * image copy.
 *
 * Windows 10/11 ci.dll: CiInitialize either jumps to (Win7-era) or calls
 * CipInitialize, and CipInitialize stores the new option flags into the
 * global with `mov dword ptr [rip+disp32], ecx` (89 0D / 89 15) or
 * `mov dword ptr [rip+disp32], imm32` (C7 05). Walking that reference gives
 * the live address of g_CiOptions without symbols.
 *
 * The same module also implements the legacy Windows 7 `g_CiEnabled` scan
 * (the `eb 06 1d 88` reference in ntoskrnl) as a fallback; on Windows 10/11
 * that pattern does not exist and the resolver reports not-found cleanly.
 */
#ifndef BLOOK_DRVLITE_CORE_DSE_RESOLVER_H
#define BLOOK_DRVLITE_CORE_DSE_RESOLVER_H

#include "pe.h"
#include "types.h"

namespace drvlite::core {

enum class dse_code : u32 {
    ok = 0,
    bad_image,
    no_ci_initialize,
    no_entry_reference,
    no_options_reference,
    target_outside_image,
    not_found,
};

const char* dse_code_str(dse_code code);

struct dse_resolution {
    dse_code code = dse_code::not_found;
    u64 options_va = 0;    /* absolute virtual address of g_CiOptions */
    u32 options_rva = 0;   /* RVA inside the image */
    u32 reference_rva = 0; /* RVA of the instruction that referenced it */
    u32 via = 0;           /* 1 = direct mov [rip], 2 = mov [rip], imm32 */
};

/* Resolves g_CiOptions in a loaded ci.dll image mapped at `image_base`. */
dse_resolution dse_resolve_ci_options(const u8* image, usize size,
                                      u64 image_base);

/* Legacy g_CiEnabled reference in ntoskrnl (Windows 7 era). */
dse_resolution dse_resolve_legacy_ci_enabled(const u8* image, usize size,
                                             u64 image_base);

}  // namespace drvlite::core

#endif /* BLOOK_DRVLITE_CORE_DSE_RESOLVER_H */
