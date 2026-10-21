# Evidence policy

`docs/evidence/` holds the output of **real** VM e2e runs. Nothing here is
synthetic: every directory is produced by `scripts/e2e.ps1` against a bvmenv
VMware guest, and `result.json` has no `dry_run` field because dry runs are
not accepted as evidence.

## Layout

```
docs/evidence/<yyyyMMdd>-drv-lite/
  result.json     # bvmenv.e2e.v1: cases, pass/fail, artifacts
  cases.txt       # chronological log of every guest command + output
  state.json      # host-side bvmenv state snapshot after the run
  screen.png      # guest screenshot (BSOD/hang classification artifact)
  target.json     # descriptor of the test process (pid, tid, buffer)
  bench.txt       # drvlite-cli bench output (latency/throughput)
```

## Rules

- A run is valid only if the VM name passed to `-Vm` is a bvmenv-managed guest
  fork; `scripts/e2e.ps1` refuses host-like names and there is no host
  fallback.
- Kernel experiments are never executed on the physical host. The CLI and
  mapper refuse unless `DRVLITE_ALLOW_KERNEL_LIVE=1` is set by the e2e runner
  inside a guest command; `scripts/check-host-clean.ps1` verifies the host
  afterwards.
- `result.json` records the `bvmenv_version` that produced the run; only\n  bvmenv 0.1.8 or 0.1.12+ (the `guest_mkdirs` recursion bug in 0.1.9-0.1.11\n  made exec/push flaky) is trusted for evidence.\n- Large raw logs are kept outside the repository (or reduced to head/tail plus
  a sha256) so the repo stays small; the JSON + cases log + screenshot are the
  canonical artifacts.
- A run with any failing case has `"pass": false`; the VM is reset and the
  failure is recorded with the guest output that caused it.

## Reproducing

```powershell
# host: build + sign + VM lifecycle + push + run + reset
./scripts/e2e.ps1 -Vm vm-drv-lite
# fast iteration against an already-provisioned VM
./scripts/e2e.ps1 -Vm vm-drv-lite -SkipBuild -Reuse
```

The script itself is the specification of the matrix; `docs/e2e.md` documents
each case and its pass criterion.
