/*
 * blook-drv-lite - opcode dispatch.
 */
#ifndef BLOOK_DRVLITE_DRIVER_DISPATCH_H
#define BLOOK_DRVLITE_DRIVER_DISPATCH_H

#include "nt.h"

namespace drvlite {

/* kchannel handler for every application opcode. */
kc_status drvlite_dispatch(void* context, u16 opcode, const void* input,
                           u32 input_length, void* output, u32* output_capacity,
                           u32* output_length);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_DISPATCH_H */
