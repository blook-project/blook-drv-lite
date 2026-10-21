# Compatibility matrix

blook-drv-lite targets Windows 10 19041+ and Windows 11 x64. Everything is
feature-detected at runtime; an unsupported build fails with a status code,
never with a bugcheck. `query_info().features` is the authoritative capability
bitmap for a session.

## Feature matrix

| Feature | Win10 19041 / 19045 | Win11 22000 | Win11 22621 / 22631 | Win11 26100 (reference) |
|---|---|---|---|---|
| manual-map load, kchannel session | yes | yes | yes | yes |
| physical read (`MmCopyMemory`) | yes | yes | yes | yes |
| physical write (`MmMapIoSpaceEx`) | yes | yes | yes | yes |
| kernel virtual read/write via page tables | yes | yes | yes | yes |
| process read/write via CR3 + PEB validation | yes | yes | yes | yes |
| 4 KiB / 2 MiB / 1 GiB page walks | yes | yes | yes | yes |
| kernel/process module enumeration + dumps | yes | yes | yes | yes |
| DSE `g_CiOptions` resolve/patch/restore | yes | yes | yes | yes |
| legacy `g_CiEnabled` fallback (Win7 era) | if present | if present | if present | not present |
| ObReference handle `GrantedAccess` rewrite | yes | yes | yes | yes |
| InfinityHook (LSTAR trampoline) | layout-checked | layout-checked | layout-checked | yes |
| DR hiding for `NtGetContextThread` | layout-checked | layout-checked | layout-checked | yes |
| syscall shield | layout-checked | layout-checked | layout-checked | yes |

"layout-checked" means the driver verifies the expected `KiSystemCall64`
prologue (`swapgs`/KPCR loads) and dispatch pattern (`mov rax, r10; call rax`)
at install time. If a build (or a mitigation such as KVA shadow) differs, the
operation returns `DRVLITE_ERR_UNSUPPORTED` and leaves `IA32_LSTAR` untouched.

## Known incompatible configurations and how they fail

| Configuration | Effect | Failure mode |
|---|---|---|
| KVA shadow (`KiSystemCall64Shadow` as LSTAR target, shadow page tables) | InfinityHook cannot place the trampoline in the shadow mapping; the reference e2e guest is such a build | `INFINITY_INSTALL` → `DRVLITE_ERR_UNSUPPORTED` (`16`); e2e records this case as `skip` with the reason, never a crash |
| HVCI / VBS memory integrity | DSE patch may be rejected or ignored by the platform; code pages may be protected | `DSE_SET` → `DRVLITE_ERR_VERIFY` (read-back mismatch) or `DRVLITE_ERR_UNSUPPORTED`; memory r/w may still work |
| test-signing enabled guest | unsigned images load even with DSE enabled, so a "blocked baseline" cannot be demonstrated | e2e gates the loader assertion and verifies the `g_CiOptions` disable/restore transition by value (`dse-cycle`) |
| ARM64 | the driver is x64 only | will not load |
| non-canonical / invalid addresses | caller error | `DRVLITE_ERR_PARAM` / `DRVLITE_ERR_IO`, never a fault |
| process with decoy CR3 | validation against `PEB.ImageBaseAddress` fails | `MEM_PROCESS_QUERY`/read → `DRVLITE_ERR_NOT_FOUND` |
| executable pool denied by policy (`POOL_FLAG_NON_PAGED_EXECUTE`) | trampoline allocation fails | `INFINITY_INSTALL` → `DRVLITE_ERR_NOMEM` |
| no `ci.dll` in the module list | DSE resolver cannot resolve | `DSE_QUERY.supported = 0`, `flags = bad-image/no-ci-initialize` |
| debug registers not exposed through `NtGetContextThread` (some virtualized CPUs) | DR hiding cannot be observed by a scanner | e2e records `hwbp_detection_bypass` as `skip` with the reason |

## Build / toolchain matrix

| Component | Verified |
|---|---|
| Visual Studio | 2026 (18.x) — CI `windows-2025-vs2026` |
| WDK / SDK | 10.0.26100.0 (CI installs the WDK NuGet content package) |
| xmake | 3.0.8 |
| clang-format | 20.1.0 (`--dry-run --Werror` in CI) |
| kchannel ABI | 1.0, pinned commit `40a5337` |

## Validation status

Host unit tests (`drvlite-tests`, 50 cases) cover page-table math, resolvers,
decoder/detour edge cases, handle-table math, protocol layout and validation on
every CI run.

Kernel-level validation requires a bvmenv VMware guest. The e2e matrix drives
the driver through the **service-load dev path** documented by the kchannel ABI
(`sc create/start` + `Parameters\Bootstrap`, written by
`drvlite-cli run --service Drvlite`): the test mapper harness is test-only and
the production load model remains the manual-map entry (`DriverEntry` receives
a kernel copy of the bootstrap in the `DriverObject` slot). The driver detects
both modes by magic.

`docs/evidence/` records each run (result JSON, case log, state snapshot,
screenshot, bench numbers). Cases that depend on features the guest build does
not permit (InfinityHook under KVA shadow, DR visibility, unsigned-load baseline
under test-signing) are recorded as `skip` with the reason, not as failures.
