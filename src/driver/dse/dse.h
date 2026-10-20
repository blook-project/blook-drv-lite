/*
 * blook-drv-lite - DSE (Driver Signature Enforcement) override.
 *
 * ci.dll's g_CiOptions is located from the live image (see
 * src/core/dse_resolver), patched through the physical write path, verified by
 * reading it back, and restored from the remembered original on shutdown.
 */
#ifndef BLOOK_DRVLITE_DRIVER_DSE_H
#define BLOOK_DRVLITE_DRIVER_DSE_H

#include "../nt.h"

namespace drvlite {

/* Resolves g_CiOptions; safe to call once during DriverEntry. */
bool dse_initialize();

/* Fills the reply with the current state. */
void dse_fill_status(drvlite_dse_rsp* response);

bool dse_available();

/* Writes `value` to g_CiOptions and verifies it. */
bool dse_set(u32 value);

bool dse_restore();

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_DSE_H */
