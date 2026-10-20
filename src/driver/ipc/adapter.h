/*
 * blook-drv-lite - kchannel transport adapter.
 *
 * This is the only file that touches blook-kchannel. Everything above it
 * talks in drvlite opcodes and buffers.
 */
#ifndef BLOOK_DRVLITE_DRIVER_IPC_ADAPTER_H
#define BLOOK_DRVLITE_DRIVER_IPC_ADAPTER_H

#include "../nt.h"

namespace drvlite {

/* Creates the transport from the bootstrap the loader passed to
 * DriverEntry and registers every application opcode. */
bool adapter_create(const kc_bootstrap* bootstrap);

/* Starts the kchannel worker after the rest of the driver is initialised. */
bool adapter_start_worker();

/* Stops and destroys the transport (safe from the worker thread). */
void adapter_stop();

/* Called by the kchannel worker when the peer dies. */
void adapter_peer_gone(void* context);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_IPC_ADAPTER_H */
