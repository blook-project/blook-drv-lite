#include "adapter.h"

#include "../dispatch.h"
#include "../state.h"

namespace drvlite {
namespace {

const u16 kApplicationOpcodes[] = {
    DRVLITE_OP_QUERY_INFO,
    DRVLITE_OP_MEM_PHYS_READ,
    DRVLITE_OP_MEM_PHYS_WRITE,
    DRVLITE_OP_MEM_KERNEL_READ,
    DRVLITE_OP_MEM_KERNEL_WRITE,
    DRVLITE_OP_MEM_PROCESS_READ,
    DRVLITE_OP_MEM_PROCESS_WRITE,
    DRVLITE_OP_MEM_BATCH_READ,
    DRVLITE_OP_MEM_PROCESS_QUERY,
    DRVLITE_OP_MODULE_LIST,
    DRVLITE_OP_MODULE_DUMP,
    DRVLITE_OP_DSE_QUERY,
    DRVLITE_OP_DSE_SET,
    DRVLITE_OP_DSE_RESTORE,
    DRVLITE_OP_HANDLE_OPEN,
    DRVLITE_OP_HANDLE_CLOSE,
    DRVLITE_OP_INFINITY_STATUS,
    DRVLITE_OP_INFINITY_INSTALL,
    DRVLITE_OP_INFINITY_REMOVE,
    DRVLITE_OP_HWBP_HIDE_SET,
    DRVLITE_OP_SYSCALL_SHIELD_SET,
    DRVLITE_OP_SHUTDOWN,
};

}  // namespace

bool adapter_create(const kc_bootstrap* bootstrap) {
    driver_state& global = state();
    kc_kernel_config config{};
    config.max_payload = DRVLITE_MAX_TRANSFER;
    config.poll_spin_us = 50;
    config.poll_idle_us = 1000;
    config.poll_idle_max_us = 50000;
    config.watchdog_ms = 5000;
    config.flags = 0;
    config.on_peer_gone = &adapter_peer_gone;
    config.ctx = &global;
    kc_kernel* kernel = nullptr;
    const kc_status status = kc_kernel_create(bootstrap, &config, &kernel);
    if (status != KC_OK || kernel == nullptr) {
        return false;
    }
    global.kchannel = kernel;
    for (const u16 opcode : kApplicationOpcodes) {
        if (kc_kernel_register(kernel, opcode, &drvlite_dispatch, &global) !=
            KC_OK) {
            kc_kernel_destroy(kernel);
            global.kchannel = nullptr;
            return false;
        }
    }
    return true;
}

bool adapter_start_worker() {
    driver_state& global = state();
    if (global.kchannel == nullptr) {
        return false;
    }
    if (kc_kernel_start(global.kchannel) != KC_OK) {
        kc_kernel_destroy(global.kchannel);
        global.kchannel = nullptr;
        return false;
    }
    return true;
}

void adapter_stop() {
    driver_state& global = state();
    if (global.kchannel != nullptr) {
        kc_kernel* kernel = global.kchannel;
        global.kchannel = nullptr;
        kc_kernel_stop(kernel);
        kc_kernel_destroy(kernel);
    }
}

void adapter_peer_gone(void* context) {
    auto* global = static_cast<driver_state*>(context);
    if (global == nullptr) {
        return;
    }
    cleanup_all();
    adapter_stop();
}

}  // namespace drvlite
