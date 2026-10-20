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

[Unreleased]: https://github.com/blook-project/blook-drv-lite/commits/main
