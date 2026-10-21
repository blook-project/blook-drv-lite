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

[Unreleased]: https://github.com/blook-project/blook-drv-lite/commits/main
