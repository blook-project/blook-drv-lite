# Application protocol (drvlite v1.0)

The driver and the SDK speak a small application protocol on top of
[blook-kchannel](https://github.com/blook-project/blook-kchannel). Every
message is one kchannel `CALL` on an application opcode (`>= 0x8000`) whose
payload is a packed structure starting with `drvlite_req_header` /
`drvlite_rsp_header` (see `include/blook/drvlite/protocol.h`).

- ABI: `DRVLITE_ABI_MAJOR 1`, `DRVLITE_ABI_MINOR 0`.
- Request header: magic `'DEQ1'`, ABI, opcode, reserved, `size`.
- Reply header: magic `'DSP1'`, ABI, opcode, status, `size`.
- The receiver rejects any message whose magic, version, opcode or `size`
  does not match the expected structure (`DRVLITE_ERR_MAGIC`,
  `DRVLITE_ERR_VERSION`, `DRVLITE_ERR_PARAM`).
- Maximum payload in one direction: `DRVLITE_MAX_TRANSFER = 0xFC00` (63 KiB).
- Batch requests: at most `DRVLITE_MAX_BATCH_RANGES = 32` ranges.

Status values are listed in `protocol.h`; 0..16 deliberately match
`kc_status` so the transport can carry them unchanged, 17..20 are
application specific and are reported inside replies/batch status arrays.

## Opcode table

| Opcode | Name | Request | Reply | Notes |
|---|---|---|---|---|
| 0x8000 | QUERY_INFO | `drvlite_query_info_req` (24 B) | `drvlite_query_info_rsp` (104 B) | build/ABI/features/base/sizes; a diagnostic page VA+PA is reported so e2e can exercise the physical path |
| 0x8010 | MEM_PHYS_READ | `drvlite_mem_req` {address,size,flags} | `mem_rsp` header + data | `MmCopyMemory(MM_COPY_MEMORY_PHYSICAL)`, page-chunked |
| 0x8011 | MEM_PHYS_WRITE | `mem_req` + data | `mem_rsp` header | `MmMapIoSpaceEx(PAGE_READWRITE|PAGE_NOCACHE)`, page-chunked |
| 0x8012 | MEM_KERNEL_READ | `mem_req` | header + data | page-table walk of the current kernel CR3 |
| 0x8013 | MEM_KERNEL_WRITE | `mem_req` + data | header | walk + physical write (works on read-only `.text`) |
| 0x8014 | MEM_PROCESS_READ | `drvlite_process_mem_req` {pid,size,address,flags} | header + data | validated CR3 walk, no attach |
| 0x8015 | MEM_PROCESS_WRITE | `process_mem_req` + data | header | same, physical write |
| 0x8016 | MEM_BATCH_READ | `drvlite_batch_read_req` {pid,count,total,flags,ranges[32]} | `drvlite_batch_read_rsp` {count,total,failed,status[32],data} | per-range status; failed ranges are zero-filled |
| 0x8017 | MEM_PROCESS_QUERY | `{pid}` | `{pid,flags,eprocess,cr3,peb,image_base,image_size}` | flags: `CR3_VALIDATED`; replies `DRVLITE_ERR_NOT_FOUND` when validation fails |
| 0x8020 | MODULE_LIST | `{pid,max_count,flags}` (`pid=0` = kernel; `flags` = start index for pagination) | `{pid,count,total,flags,modules[]}` | kernel: `PsLoadedModuleList`; process: PEB `Ldr` via CR3; `total` is the full count, `count` the returned window |
| 0x8021 | MODULE_DUMP | `{pid,size,address,flags}` | `{pid,size,copied,flags,address,data}` | tolerant read: holes are zero-filled |
| 0x8030 | DSE_QUERY | `drvlite_dse_req` | `drvlite_dse_rsp` | supported/patched/current/original/address/ci.dll info |
| 0x8031 | DSE_SET | `{value}` | same as query | patch + read-back verification |
| 0x8032 | DSE_RESTORE | `{}` | same as query | writes `original` back |
| 0x8040 | HANDLE_OPEN | `{pid,access,type,tid,reserved}` | `{handle,object,requested,granted,flags}` | opens a minimal handle in the client process, rewrites `GrantedAccess` to `access` (`0x1FFFFF` = full) |
| 0x8041 | HANDLE_CLOSE | `{handle,pid}` | `{closed}` | closes in the client process context |
| 0x8050 | INFINITY_STATUS | `drvlite_infinity_remove_req` (24 B) | `drvlite_infinity_status_rsp` | installed/supported/entry/dispatch/counters, `flags` = last error |
| 0x8051 | INFINITY_INSTALL | `{filter_count,flags,ssn_filter[16]}` | status | per-CPU `IA32_LSTAR` trampoline; fails cleanly when unsupported |
| 0x8052 | INFINITY_REMOVE | `{}` | status | restores every CPU's original LSTAR |
| 0x8053 | HWBP_HIDE_SET | `{enable,pid,get_context_ssn,flags}` | `{enabled,pid,ssn,hidden_queries}` | zeroes `Dr0-3/Dr6/Dr7` in `NtGetContextThread` replies for the protected process |
| 0x8054 | SYSCALL_SHIELD_SET | `{enable,pid,ssn_count,flags,ssns[16]}` | `{enabled,pid,blocked_calls}` | denied SSNs return `STATUS_ACCESS_DENIED` without dispatch |
| 0x8070 | SHUTDOWN | `{}` | `{stage,restored}` | restores hooks/DSE, stops the worker, destroys the transport |

## Limits and validation

| Item | Rule |
|---|---|
| transfer size | `1 <= size <= 0xFC00`, and request length must equal header size for writes |
| canonical addresses | kernel addresses `>= 0xFFFF800000000000`, user addresses `< 0x0000800000000000` (non-canonical rejected) |
| user ranges | both ends must be canonical user addresses; process read/write never touches kernel ranges |
| batch | `1 <= count <= 32`, every size non-zero, total `<= 0xFC00`, no overflow |
| modules | at most 512 modules per list reply; names truncated to 255 chars |
| handles | handle value must be 4-byte aligned; index bounded by `1 << 22` |
| InfinityHook filters | `filter_count <= 16`; SSNs masked to 12 bits |

Every handler validates before touching memory and returns a status instead
of faulting. The host unit tests cover the shared validation code
(`src/core/protocol_codec.cpp`) with malformed headers and batch limits.

## Versioning

Breaking changes bump `DRVLITE_ABI_MINOR` (or major) and are announced to all
consumers. Adding a new opcode is not a breaking change; extending a request
requires a new opcode or a size/version guard because `size` must match the
receiver's structure exactly.
