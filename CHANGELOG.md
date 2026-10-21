# Changelog

All notable changes to this project are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Repository scaffold, xmake build, host unit-test harness, CI/release
  workflows and package-registry manifest.
- `drvlite` protocol header and shared portable core (page-table walk,
  physical-range chunking, PE/pattern resolver, DSE `g_CiOptions` resolver,
  handle-table math, x86 length decoder, detour builder).
- Manual-map kernel driver: kchannel IPC adapter, physical/kernel-virtual/
  process memory read/write, module enumeration and dumps, DSE override,
  ObReference handle bypass, InfinityHook system-call interception with
  hardware-breakpoint detection bypass.
- User-mode SDK and `drvlite-cli`.
- SDK guide/protocol/compatibility/safety/performance/release docs, CLI JSON
  output, and a buildable worked example (`examples/read_process_dse.cpp`).
- VM e2e runner (`scripts/e2e.ps1`) for the bvmenv environment: manual-map
  load, physical/kernel/process memory with page-crossing and large-page
  cases, DSE disable/restore while an unsigned driver loads, full-permission
  handle (including a protected-process attempt), InfinityHook counters and
  debug-register hiding, shutdown with hooks armed, 1000 load/unload cycles,
  malformed-input fuzzing, host-side state/screenshot evidence.
- Host-safety guards: kernel code only runs inside a bvmenv guest
  (`DRVLITE_ALLOW_KERNEL_LIVE`), `-Vm` is mandatory in the e2e runner, and
  `scripts/check-host-clean.ps1` verifies the host has no driver residue.
- Host unit tests for protocol layout/wire sizes, malformed/oversized
  requests, decoder relocation failure paths, 4 KiB/2 MiB/1 GiB page walks,
  DSE resolver fixtures and handle-table bitfield math.
- Module-list pagination and a diagnostic physical page for direct
  physical-path verification.

## [1.0.1] - 2026-10-21

### Fixed

- The 1000x load/unload e2e case is now a fast SCM-level `loadcycle` loop, so
  it finishes well inside the case timeout and validates repeated image
  load/unload rather than being dominated by per-cycle CLI startup.
- The process-level cycle case retries an SCM start-pending race once per
  iteration instead of failing a healthy cycle.
- The trace scan unloads the service before checking for leftovers.

## [1.0.0] - 2026-10-21

### Fixed

- DriverEntry deadlock under the SCM load path: the 32 MiB backwards PE scan
  of the driver's own image and the `PsLoadedModuleList` walk were replaced
  with `RtlPcToFileHeader`/`RtlImageNtHeader` and
  `MmGetSystemRoutineAddress`; the client process lookup and diagnostic page
  moved to a deferred PASSIVE system thread.
- Service-load bootstrap delivery now stamps `peer_pid` with the CLI process
  (a zero value made kchannel resolve the shared region in the system
  process and fail with `KC_ERR_MAP`).
- Full-permission handles are opened directly with `ObOpenObjectByPointer` in
  kernel mode (no handle-table offset probing needed); the table-rewrite path
  remains as fallback.
- Large (63 KiB) kernel/process transfers now fit the kchannel payload budget,
  and byte lists may be passed as one quoted argument or several tokens.
- Stress/bench process memory: buffers are touched before measurement so the
  CR3 walk sees present pages.
- e2e runner captures tolerated guest output, records `skip` reasons, and
  gates InfinityHook/DR-hiding cases on guest builds where the syscall entry
  pattern is unsupported (KVA shadow).

[Unreleased]: https://github.com/blook-project/blook-drv-lite/commits/main
