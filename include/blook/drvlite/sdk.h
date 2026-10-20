/*
 * blook-drv-lite - user-mode SDK.
 *
 * The SDK owns the kchannel session. The load model is two-phase because the
 * driver image has to be mapped between the two calls:
 *
 *   drvlite::session session;
 *   session.attach(options);              // creates the section + keys
 *   kc_bootstrap boot = session.bootstrap();
 *   <manual-map loader>  passes a KERNEL COPY of `boot` as its entry argument
 *   session.finish();                     // handshake -> ACTIVE
 *   session.read_process(pid, address, buffer, size);
 *
 * All calls return a drvlite status code (DRVLITE_OK, DRVLITE_ERR_*) or a
 * kchannel transport error; `status_string()` renders them.
 */
#ifndef BLOOK_DRVLITE_SDK_H
#define BLOOK_DRVLITE_SDK_H

#include <windows.h>

#include <blook/drvlite/protocol.h>
#include <blook/kchannel.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace drvlite {

struct attach_options {
    uint32_t region_size = 0x40000; /* enough for full 64 KiB payloads */
    uint32_t mode = KC_MODE_MAPPED;
    uint32_t cipher = KC_CIPHER_CHACHA20;
    uint32_t peer_pid = 0; /* current process */
    uint32_t flags = 0;
};

struct device_info {
    uint32_t nt_build = 0;
    uint32_t abi = 0;
    uint32_t features = 0;
    uint64_t driver_base = 0;
    uint32_t driver_size = 0;
    uint64_t kernel_base = 0;
    uint32_t kernel_size = 0;
    uint64_t client_eprocess = 0;
    uint32_t client_pid = 0;
};

struct process_details {
    uint32_t pid = 0;
    uint32_t flags = 0;
    uint64_t eprocess = 0;
    uint64_t cr3 = 0;
    uint64_t peb = 0;
    uint64_t image_base = 0;
    uint32_t image_size = 0;
    bool cr3_validated = false;
};

struct dse_status {
    bool supported = false;
    bool patched = false;
    uint32_t current = 0;
    uint32_t original = 0;
    uint32_t nt_build = 0;
    uint32_t error = 0;
    uint64_t address = 0;
    uint64_t ci_base = 0;
    uint32_t ci_size = 0;
    uint32_t via = 0;
};

struct hook_status {
    bool installed = false;
    bool supported = false;
    uint64_t entry = 0;
    uint64_t dispatch = 0;
    uint64_t total_syscalls = 0;
    uint64_t filtered_hits = 0;
    uint64_t handled_calls = 0;
    uint32_t spliced_bytes = 0;
    uint32_t flags = 0;
};

class session {
   public:
    session();
    ~session();
    session(const session&) = delete;
    session& operator=(const session&) = delete;

    /* Creates the anonymous section + per-attach keys (kc_attach). */
    int attach(const attach_options& options = attach_options{});

    /* Bootstrap descriptor to hand to the manual-map loader. */
    const kc_bootstrap& bootstrap() const { return bootstrap_; }

    /* Completes the handshake after the image is mapped. */
    int finish(uint32_t timeout_ms = 5000);

    void detach();

    bool active() const { return active_; }

    /* Raw application-protocol call. */
    int call(uint16_t opcode, const void* input, uint32_t input_length,
             void* output, uint32_t output_capacity, uint32_t* output_length,
             uint32_t timeout_ms = 5000);

    /* Session / introspection. */
    int query_info(device_info* out) const;
    int process_info(uint32_t pid, process_details* out) const;
    int modules(uint32_t pid, std::vector<drvlite_module_info>* out) const;
    int module_dump(uint32_t pid, uint64_t address, uint32_t size,
                    std::vector<uint8_t>* data, uint32_t* bytes_read) const;

    /* Memory. */
    int read_physical(uint64_t address, void* buffer, uint32_t size) const;
    int write_physical(uint64_t address, const void* buffer, uint32_t size);
    int read_kernel(uint64_t address, void* buffer, uint32_t size) const;
    int write_kernel(uint64_t address, const void* buffer, uint32_t size);
    int read_process(uint32_t pid, uint64_t address, void* buffer,
                     uint32_t size) const;
    int write_process(uint32_t pid, uint64_t address, const void* buffer,
                      uint32_t size);
    int batch_read_process(uint32_t pid, const drvlite_range* ranges,
                           uint32_t count, std::vector<uint8_t>* data,
                           std::vector<uint32_t>* statuses) const;

    /* DSE. */
    int dse_query(dse_status* out) const;
    int dse_set(uint32_t value);
    int dse_restore();

    /* Handles: open a handle in the client process with full access. */
    int open_process_handle(uint32_t pid, uint32_t access, HANDLE* out,
                            uint32_t* granted);
    int open_thread_handle(uint32_t pid, uint32_t tid, uint32_t access,
                           HANDLE* out, uint32_t* granted);
    int close_handle(HANDLE handle, uint32_t pid);

    /* InfinityHook. */
    int infinity_status(hook_status* out) const;
    int infinity_install(const std::vector<uint32_t>& ssn_filter = {});
    int infinity_remove();
    int hide_debug_registers(bool enable, uint32_t pid);
    int set_syscall_shield(bool enable, uint32_t pid,
                           const std::vector<uint32_t>& ssns);

    /* Restores every patch and stops the driver worker. */
    int shutdown();

   private:
    int call_with_reply(uint16_t opcode, const void* input,
                        uint32_t input_length, void* output,
                        uint32_t output_capacity, uint32_t min_reply_size,
                        uint32_t* output_length,
                        uint32_t timeout_ms = 5000) const;
    int device_call(uint16_t opcode, const void* input, uint32_t input_length,
                    void* output, uint32_t output_capacity,
                    uint32_t* output_length, uint32_t min_reply_size) const;

    kc_client* client_ = nullptr;
    kc_bootstrap bootstrap_{};
    bool active_ = false;
    bool attached_ = false;
    std::vector<std::pair<HANDLE, uint32_t>> handles_;
};

/* Resolves a syscall number from the local ntdll stub ("NtGetContextThread").
 */
uint32_t ntdll_syscall_number(const char* export_name);

/* Human readable status code. */
const char* status_string(int status);

/* Version string ("0.1.0"). */
const char* version_string();

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_SDK_H */
