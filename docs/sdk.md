# SDK guide

`include/blook/drvlite/sdk.h` is the supported user-mode API. It is a thin
typed layer over the kchannel session and the application protocol
([protocol.md](protocol.md)); error codes are the `DRVLITE_*` values from
`protocol.h` (`status_string()` renders them).

## Load model

The driver is manual-map-only, so loading is two-phase (the image has to be
mapped between the two handshake steps):

```cpp
#include <blook/drvlite/sdk.h>

drvlite::session session;
drvlite::attach_options options;
options.region_size = 0x40000; /* enough for full 64 KiB payloads */
int status = session.attach(options);
if (status != DRVLITE_OK) { /* kc_attach failed */ }

kc_bootstrap bootstrap = session.bootstrap(); /* descriptor, not a secret */
map_driver_image(session.bootstrap());        /* loader passes a KERNEL copy */

status = session.finish(5000);                /* HELLO/HELLO_ACK -> ACTIVE */
```

RAII: `session` rolls back on destruction (`kc_detach`, handles closed).
`session.shutdown()` asks the driver to restore every kernel patch and stop
its worker before the loader unmaps the image.

## Worked example

```cpp
#include <blook/drvlite/sdk.h>

int main(int argc, char** argv) {
    const uint32_t pid = static_cast<uint32_t>(std::stoul(argv[1]));
    const uint64_t address = std::stoull(argv[2], nullptr, 0);

    drvlite::session session;
    if (session.attach() != DRVLITE_OK) return 1;
    /* ... map drvlite.sys with session.bootstrap() ... */
    if (session.finish(5000) != DRVLITE_OK) return 1;

    drvlite::device_info info;
    session.query_info(&info);
    std::printf("drv-lite %s, nt build %u\n", drvlite::version_string(),
                info.nt_build);

    /* read 0x100 bytes of a running process through the CR3 walk */
    uint8_t bytes[0x100];
    if (session.read_process(pid, address, bytes, sizeof(bytes)) == DRVLITE_OK) {
        std::printf("read %zu bytes, first byte 0x%02x\n", sizeof(bytes),
                    bytes[0]);
    }

    /* temporarily allow unsigned drivers */
    drvlite::dse_status dse;
    if (session.dse_query(&dse) == DRVLITE_OK && dse.supported) {
        session.dse_set(0);
        /* load the unsigned test driver here */
        session.dse_restore();
    }

    session.shutdown();
    return 0;
}
```

A complete buildable program is in `examples/read_process_dse.cpp` and is
built by xmake as `drvlite-example`.

## API summary

| Group | Functions |
|---|---|
| session | `attach`, `bootstrap`, `finish`, `detach`, `active`, `call` |
| introspection | `query_info`, `process_info`, `modules`, `module_dump` |
| memory | `read_physical`, `write_physical`, `read_kernel`, `write_kernel`, `read_process`, `write_process`, `batch_read_process` |
| code integrity | `dse_query`, `dse_set`, `dse_restore` |
| handles | `open_process_handle`, `open_thread_handle`, `close_handle` |
| InfinityHook | `infinity_status`, `infinity_install`, `infinity_remove`, `hide_debug_registers`, `set_syscall_shield` |
| lifecycle | `shutdown` |
| helpers | `ntdll_syscall_number(name)`, `status_string`, `version_string` |

Capability detection: `device_info.features` bitmask (`DRVLITE_FEATURE_*`)
is authoritative. An operation whose feature is absent fails with
`DRVLITE_ERR_UNSUPPORTED`, never with a crash. Run `drvlite-cli run ... -- info`
to print it.

## Notes

- `open_process_handle(pid, DRVLITE_ACCESS_MINIMAL, ...)` asks for minimal
  access and returns a handle whose `GrantedAccess` was rewritten to
  `access`. Pass `DRVLITE_ACCESS_FULL` (`0x1FFFFF`) for full rights.
- `hide_debug_registers` resolves the `NtGetContextThread` syscall number
  from the local ntdll stub, so it works across builds without hard-coded
  SSNs.
- All functions are synchronous. Memory transfer is capped at 63 KiB per
  call; loop or use `batch_read_process` for larger ranges.
