/*
 * blook-drv-lite - application protocol shared by the kernel driver and the
 * user-mode SDK.
 *
 * Every message is one kchannel CALL payload (application opcodes >= 0x8000).
 * The first member of each request is drvlite_req_header and the first member
 * of each reply is drvlite_rsp_header; both carry a magic, the ABI version,
 * the opcode and the total structure length so a stale or malformed peer is
 * rejected before any field is used.
 *
 * MIT licensed; authorized security research only.
 */
#ifndef BLOOK_DRVLITE_PROTOCOL_H
#define BLOOK_DRVLITE_PROTOCOL_H

/*
 * Fixed width types.
 *
 * The header is self-sufficient: the kernel build compiles the portable core
 * translation units with /kernel without necessarily including the WDK headers
 * first. kchannel_km.h is the authoritative kernel definition of the standard
 * fixed width names; user mode (and the host tests) use the CRT.
 */
#if defined(_KERNEL_MODE)
#include <ntddk.h>
#include <blook/kchannel_km.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define DRVLITE_PROTOCOL_MAGIC 0x31504C44u /* 'DLP1' */
#define DRVLITE_ABI_MAJOR 1u
#define DRVLITE_ABI_MINOR 0u
#define DRVLITE_ABI_VERSION_STRING "1.0.0"

/* Request/reply header magic values (different so a reply can never be
 * mistaken for a request by a confused peer). */
#define DRVLITE_REQ_MAGIC 0x31514544u /* 'DEQ1' */
#define DRVLITE_RSP_MAGIC 0x31505344u /* 'DSP1' */

/* Maximum payload the driver accepts in one request and returns in one reply.
 * The SDK creates a kchannel region of 0x40000 bytes, which leaves room for
 * the transport headers plus this many bytes. */
#define DRVLITE_MAX_TRANSFER 0xFC00u

/* Hard limits for variable-length structures. */
#define DRVLITE_MAX_BATCH_RANGES 32u
#define DRVLITE_MAX_SSN_FILTER 16u
#define DRVLITE_MODULE_NAME_MAX 256u
#define DRVLITE_MAX_MODULES 512u

/* Features reported by DRVLITE_OP_QUERY_INFO. */
#define DRVLITE_FEATURE_PHYS_READ 0x00000001u
#define DRVLITE_FEATURE_PHYS_WRITE 0x00000002u
#define DRVLITE_FEATURE_KERNEL_MEMORY 0x00000004u
#define DRVLITE_FEATURE_PROCESS_MEMORY 0x00000008u
#define DRVLITE_FEATURE_MODULES 0x00000010u
#define DRVLITE_FEATURE_DSE 0x00000020u
#define DRVLITE_FEATURE_HANDLES 0x00000040u
#define DRVLITE_FEATURE_INFINITY_HOOK 0x00000080u
#define DRVLITE_FEATURE_HWBP_HIDE 0x00000100u
#define DRVLITE_FEATURE_SYSCALL_SHIELD 0x00000200u

/* drvlite_mem_* / drvlite_module_dump flags. */
#define DRVLITE_MEM_FLAG_TOLERANT \
    0x00000001u /* zero-fill holes instead of failing */
#define DRVLITE_MEM_FLAG_VERIFY 0x00000002u /* read back and verify writes */

/* drvlite_process_query_rsp.flags */
#define DRVLITE_PROCESS_FLAG_CR3_VALIDATED 0x00000001u
#define DRVLITE_PROCESS_FLAG_CR3_CACHED 0x00000002u

/* drvlite_handle_open_req.type */
#define DRVLITE_HANDLE_TYPE_PROCESS 0u
#define DRVLITE_HANDLE_TYPE_THREAD 1u

/* drvlite_handle_open_req.access presets. */
#define DRVLITE_ACCESS_MINIMAL \
    0x00001000u /* PROCESS_QUERY_LIMITED_INFORMATION */
#define DRVLITE_ACCESS_FULL \
    0x001FFFFFu                 /* PROCESS_ALL_ACCESS / THREAD_ALL_ACCESS */
#define DRVLITE_ACCESS_NEVER 0u /* open with no access at all */

#if defined(__cplusplus)
extern "C" {
#endif

#pragma pack(push, 8)

typedef enum drvlite_op {
    DRVLITE_OP_QUERY_INFO = 0x8000,
    DRVLITE_OP_MEM_PHYS_READ = 0x8010,
    DRVLITE_OP_MEM_PHYS_WRITE = 0x8011,
    DRVLITE_OP_MEM_KERNEL_READ = 0x8012,
    DRVLITE_OP_MEM_KERNEL_WRITE = 0x8013,
    DRVLITE_OP_MEM_PROCESS_READ = 0x8014,
    DRVLITE_OP_MEM_PROCESS_WRITE = 0x8015,
    DRVLITE_OP_MEM_BATCH_READ = 0x8016,
    DRVLITE_OP_MEM_PROCESS_QUERY = 0x8017,
    DRVLITE_OP_MODULE_LIST = 0x8020,
    DRVLITE_OP_MODULE_DUMP = 0x8021,
    DRVLITE_OP_DSE_QUERY = 0x8030,
    DRVLITE_OP_DSE_SET = 0x8031,
    DRVLITE_OP_DSE_RESTORE = 0x8032,
    DRVLITE_OP_HANDLE_OPEN = 0x8040,
    DRVLITE_OP_HANDLE_CLOSE = 0x8041,
    DRVLITE_OP_INFINITY_STATUS = 0x8050,
    DRVLITE_OP_INFINITY_INSTALL = 0x8051,
    DRVLITE_OP_INFINITY_REMOVE = 0x8052,
    DRVLITE_OP_HWBP_HIDE_SET = 0x8053,
    DRVLITE_OP_SYSCALL_SHIELD_SET = 0x8054,
    DRVLITE_OP_SHUTDOWN = 0x8070
} drvlite_op;

/*
 * Application status values. Values 0..16 deliberately equal kchannel's
 * kc_status so a handler can return them directly; 17+ are application
 * specific and are reported through the reply header status field (the SDK
 * translates them back).
 */
typedef enum drvlite_status {
    DRVLITE_OK = 0,
    DRVLITE_ERR_PARAM = 1,
    DRVLITE_ERR_NOMEM = 2,
    DRVLITE_ERR_MAGIC = 3,
    DRVLITE_ERR_VERSION = 4,
    DRVLITE_ERR_STATE = 5,
    DRVLITE_ERR_AUTH = 6,
    DRVLITE_ERR_TIMEOUT = 7,
    DRVLITE_ERR_BUSY = 8,
    DRVLITE_ERR_IO = 9,
    DRVLITE_ERR_MAP = 10,
    DRVLITE_ERR_NOT_FOUND = 11,
    DRVLITE_ERR_ALREADY = 12,
    DRVLITE_ERR_INTERNAL = 13,
    DRVLITE_ERR_PEER_GONE = 14,
    DRVLITE_ERR_HANDLER = 15,
    DRVLITE_ERR_UNSUPPORTED = 16,
    DRVLITE_ERR_RANGE = 17,
    DRVLITE_ERR_ACCESS = 18,
    DRVLITE_ERR_VERIFY = 19,
    DRVLITE_ERR_NOT_LOADED = 20
} drvlite_status;

const char* drvlite_status_str(int status);

typedef struct drvlite_req_header {
    uint32_t magic;     /* DRVLITE_REQ_MAGIC */
    uint16_t abi_major; /* DRVLITE_ABI_MAJOR */
    uint16_t abi_minor; /* DRVLITE_ABI_MINOR */
    uint16_t op;        /* drvlite_op */
    uint16_t reserved;  /* must be 0 */
    uint32_t size;      /* sizeof(request struct) */
} drvlite_req_header;

typedef struct drvlite_rsp_header {
    uint32_t magic;     /* DRVLITE_RSP_MAGIC */
    uint16_t abi_major; /* DRVLITE_ABI_MAJOR */
    uint16_t abi_minor; /* DRVLITE_ABI_MINOR */
    uint16_t op;        /* drvlite_op that produced this reply */
    uint16_t reserved;  /* must be 0 */
    uint32_t status;    /* drvlite_status */
    uint32_t size;      /* total reply bytes written */
} drvlite_rsp_header;

/* ------------------------------------------------------------------ */
/* QUERY_INFO                                                         */
/* ------------------------------------------------------------------ */

typedef struct drvlite_query_info_req {
    drvlite_req_header hdr;
    uint32_t reserved;
    uint32_t reserved2;
} drvlite_query_info_req;

typedef struct drvlite_query_info_rsp {
    drvlite_rsp_header hdr;
    uint32_t nt_build;
    uint32_t abi;
    uint32_t features;
    uint32_t flags;
    uint64_t driver_base;
    uint64_t kernel_base;
    uint32_t driver_size;
    uint32_t kernel_size;
    uint64_t client_eprocess;
    uint32_t client_pid;
    uint32_t reserved;
    /* Diagnostic page owned by the driver: lets the e2e suite prove direct
     * physical read/write without guessing a physical address. */
    uint64_t test_page_va;
    uint64_t test_page_pa;
    uint32_t reserved2;
    uint32_t reserved3;
} drvlite_query_info_rsp;

/* ------------------------------------------------------------------ */
/* Memory                                                             */
/* ------------------------------------------------------------------ */

typedef struct drvlite_mem_req {
    drvlite_req_header hdr;
    uint64_t address;
    uint32_t size;
    uint32_t flags;
} drvlite_mem_req;

/* Inline payload follows a mem request for writes; replies follow the header
 * for reads. data[1] is only a marker: size says how many bytes follow. */
typedef struct drvlite_mem_rsp {
    drvlite_rsp_header hdr;
    uint64_t address;
    uint32_t size;
    uint32_t flags;
    uint8_t data[1];
} drvlite_mem_rsp;

typedef struct drvlite_process_mem_req {
    drvlite_req_header hdr;
    uint32_t pid;
    uint32_t size;
    uint64_t address;
    uint32_t flags;
    uint32_t reserved;
} drvlite_process_mem_req;

typedef struct drvlite_process_mem_rsp {
    drvlite_rsp_header hdr;
    uint32_t pid;
    uint32_t size;
    uint64_t address;
    uint32_t flags;
    uint32_t reserved;
    uint8_t data[1];
} drvlite_process_mem_rsp;

typedef struct drvlite_range {
    uint64_t address;
    uint32_t size;
    uint32_t reserved;
} drvlite_range;

typedef struct drvlite_batch_read_req {
    drvlite_req_header hdr;
    uint32_t pid;
    uint32_t count;
    uint32_t total;
    uint32_t flags;
    drvlite_range ranges[DRVLITE_MAX_BATCH_RANGES];
} drvlite_batch_read_req;

typedef struct drvlite_batch_read_rsp {
    drvlite_rsp_header hdr;
    uint32_t pid;
    uint32_t count;
    uint32_t total;
    uint32_t failed;
    uint32_t status[DRVLITE_MAX_BATCH_RANGES];
    uint8_t data[1];
} drvlite_batch_read_rsp;

typedef struct drvlite_process_query_req {
    drvlite_req_header hdr;
    uint32_t pid;
    uint32_t reserved;
} drvlite_process_query_req;

typedef struct drvlite_process_query_rsp {
    drvlite_rsp_header hdr;
    uint32_t pid;
    uint32_t flags;
    uint64_t eprocess;
    uint64_t cr3;
    uint64_t peb;
    uint64_t image_base;
    uint32_t image_size;
    uint32_t reserved;
} drvlite_process_query_rsp;

/* ------------------------------------------------------------------ */
/* Modules                                                            */
/* ------------------------------------------------------------------ */

typedef struct drvlite_module_info {
    uint64_t base;
    uint64_t size;
    uint32_t flags;
    uint32_t name_len;
    char name[DRVLITE_MODULE_NAME_MAX];
} drvlite_module_info;

typedef struct drvlite_module_list_req {
    drvlite_req_header hdr;
    uint32_t pid; /* 0 => kernel modules */
    uint32_t max_count;
    uint32_t flags;
    uint32_t reserved;
} drvlite_module_list_req;

typedef struct drvlite_module_list_rsp {
    drvlite_rsp_header hdr;
    uint32_t pid;
    uint32_t count;
    uint32_t total;
    uint32_t flags;
    drvlite_module_info modules[1];
} drvlite_module_list_rsp;

/* Module dump uses the same request shape as memory reads; the address is the
 * module base and the driver tolerates unmapped holes. */
typedef struct drvlite_module_dump_req {
    drvlite_req_header hdr;
    uint32_t pid;
    uint32_t size;
    uint64_t address;
    uint32_t flags;
    uint32_t reserved;
} drvlite_module_dump_req;

typedef struct drvlite_module_dump_rsp {
    drvlite_rsp_header hdr;
    uint32_t pid;
    uint32_t size;
    uint32_t copied;
    uint32_t flags;
    uint64_t address;
    uint8_t data[1];
} drvlite_module_dump_rsp;

/* ------------------------------------------------------------------ */
/* DSE                                                                */
/* ------------------------------------------------------------------ */

typedef struct drvlite_dse_req {
    drvlite_req_header hdr;
    uint32_t value; /* requested g_CiOptions value for DSE_SET */
    uint32_t flags;
} drvlite_dse_req;

typedef struct drvlite_dse_rsp {
    drvlite_rsp_header hdr;
    uint32_t supported;
    uint32_t patched;
    uint32_t current;
    uint32_t original;
    uint32_t nt_build;
    uint32_t flags;
    uint64_t address;
    uint64_t ci_base;
    uint32_t ci_size;
    uint32_t reserved;
} drvlite_dse_rsp;

/* ------------------------------------------------------------------ */
/* Handles                                                            */
/* ------------------------------------------------------------------ */

typedef struct drvlite_handle_open_req {
    drvlite_req_header hdr;
    uint32_t pid;
    uint32_t access;
    uint32_t type; /* DRVLITE_HANDLE_TYPE_* */
    uint32_t tid;  /* thread id when type == thread */
    uint64_t reserved;
} drvlite_handle_open_req;

typedef struct drvlite_handle_open_rsp {
    drvlite_rsp_header hdr;
    uint64_t handle;
    uint64_t object;
    uint32_t requested;
    uint32_t granted;
    uint32_t flags;
    uint32_t reserved;
} drvlite_handle_open_rsp;

typedef struct drvlite_handle_close_req {
    drvlite_req_header hdr;
    uint64_t handle;
    uint32_t pid;
    uint32_t reserved;
} drvlite_handle_close_req;

typedef struct drvlite_handle_close_rsp {
    drvlite_rsp_header hdr;
    uint32_t closed;
    uint32_t reserved;
} drvlite_handle_close_rsp;

/* ------------------------------------------------------------------ */
/* InfinityHook / debug-register hiding                               */
/* ------------------------------------------------------------------ */

typedef struct drvlite_infinity_status_rsp {
    drvlite_rsp_header hdr;
    uint32_t installed;
    uint32_t supported;
    uint64_t entry;
    uint64_t dispatch;
    uint64_t total_syscalls;
    uint64_t filtered_hits;
    uint64_t handled_calls;
    uint32_t spliced_bytes;
    uint32_t flags;
} drvlite_infinity_status_rsp;

typedef struct drvlite_infinity_install_req {
    drvlite_req_header hdr;
    uint32_t filter_count;
    uint32_t flags;
    uint32_t ssn_filter[DRVLITE_MAX_SSN_FILTER];
} drvlite_infinity_install_req;

typedef struct drvlite_infinity_remove_req {
    drvlite_req_header hdr;
    uint32_t flags;
    uint32_t reserved;
} drvlite_infinity_remove_req;

typedef struct drvlite_hwbp_hide_req {
    drvlite_req_header hdr;
    uint32_t enable;
    uint32_t pid;             /* protected process, 0 => current client pid */
    uint32_t get_context_ssn; /* NtGetContextThread SSN, resolved by the SDK */
    uint32_t flags;
} drvlite_hwbp_hide_req;

typedef struct drvlite_hwbp_hide_rsp {
    drvlite_rsp_header hdr;
    uint32_t enabled;
    uint32_t pid;
    uint32_t get_context_ssn;
    uint32_t hidden_queries;
} drvlite_hwbp_hide_rsp;

typedef struct drvlite_syscall_shield_req {
    drvlite_req_header hdr;
    uint32_t enable;
    uint32_t pid;
    uint32_t ssn_count;
    uint32_t flags;
    uint32_t ssns[DRVLITE_MAX_SSN_FILTER];
} drvlite_syscall_shield_req;

typedef struct drvlite_syscall_shield_rsp {
    drvlite_rsp_header hdr;
    uint32_t enabled;
    uint32_t pid;
    uint32_t blocked_calls;
    uint32_t reserved;
} drvlite_syscall_shield_rsp;

/* ------------------------------------------------------------------ */
/* Shutdown                                                           */
/* ------------------------------------------------------------------ */

typedef struct drvlite_shutdown_req {
    drvlite_req_header hdr;
    uint32_t flags;
    uint32_t reserved;
} drvlite_shutdown_req;

typedef struct drvlite_shutdown_rsp {
    drvlite_rsp_header hdr;
    uint32_t stage; /* 1 = patches restored, worker stopping */
    uint32_t restored;
} drvlite_shutdown_rsp;

#pragma pack(pop)

#if defined(__cplusplus)
#define DRVLITE_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define DRVLITE_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

DRVLITE_STATIC_ASSERT(sizeof(drvlite_req_header) == 16, "req header size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_rsp_header) == 20, "rsp header size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_mem_req) == 32, "mem req size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_process_mem_req) == 40,
                      "process mem req size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_batch_read_req) ==
                          32 + sizeof(drvlite_range) * DRVLITE_MAX_BATCH_RANGES,
                      "batch req size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_module_info) == 280, "module info size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_dse_rsp) == 72, "dse rsp size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_infinity_install_req) ==
                          24 + 4 * DRVLITE_MAX_SSN_FILTER,
                      "infinity install req size");
/* Wire layout pinned so kernel and SDK cannot drift silently. */
DRVLITE_STATIC_ASSERT(sizeof(drvlite_query_info_rsp) == 104,
                      "query info rsp size");
DRVLITE_STATIC_ASSERT(offsetof(drvlite_mem_rsp, data) == 40,
                      "memory reply payload offset");
DRVLITE_STATIC_ASSERT(offsetof(drvlite_process_mem_rsp, data) == 48,
                      "process memory reply payload offset");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_batch_read_req) == 544,
                      "batch read request size");
DRVLITE_STATIC_ASSERT(offsetof(drvlite_batch_read_rsp, data) == 164,
                      "batch read reply payload offset");
DRVLITE_STATIC_ASSERT(offsetof(drvlite_module_list_rsp, modules) == 40,
                      "module list payload offset");
DRVLITE_STATIC_ASSERT(offsetof(drvlite_module_dump_rsp, data) == 48,
                      "module dump payload offset");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_handle_open_rsp) == 56,
                      "handle open reply size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_infinity_status_rsp) == 80,
                      "infinity status reply size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_hwbp_hide_rsp) == 36,
                      "hwbp hide reply size");
DRVLITE_STATIC_ASSERT(sizeof(drvlite_syscall_shield_req) == 96,
                      "syscall shield request size");

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* BLOOK_DRVLITE_PROTOCOL_H */
