# Performance

The numbers below are produced by `drvlite-cli run ... -- bench` inside the
`bvmenv` test VM (Windows 11 26100 x64, 4 vCPU, kchannel COPY mode used by the
e2e service-load path, region size 0x40000, 200 iterations per operation, one
4 KiB payload per call). Measurements are wall-clock per `kc_call` round trip
including the kchannel transport, so they describe the SDK-facing cost, not
just the memory copy.

```powershell
# inside the guest (service-load dev path used by the e2e matrix)
drvlite-cli.exe run --service Drvlite --mode copy --image C:\drvlite\drvlite.sys `
    --bootstrap C:\drvlite\boot.bin --map-cmd C:\drvlite\start-drv.cmd `
    -- bench --iterations 200
```

The command prints, for each operation, the median and p99 latency and the
effective throughput for a 4 KiB transfer. `docs/evidence/<date>-drv-lite/bench.txt`
keeps the raw output of every e2e run.

## Reference table (Windows 11 26100 guest, 4 vCPU, run 2026-10-21)

| Operation | Median | p99 | Throughput |
|---|---|---|---|
| physical read 4 KiB | 73.2 us | 62.3 ms | 53.4 MiB/s |
| physical write 4 KiB | 81.5 us | 62.0 ms | 47.9 MiB/s |
| kernel virtual read 4 KiB | 71.0 us | 61.6 ms | 55.0 MiB/s |
| process read 4 KiB (CR3 walk) | 75.4 us | 62.1 ms | 51.8 MiB/s |
| process write 4 KiB | 75.4 us | 62.3 ms | 51.8 MiB/s |
| syscall entry overhead (hook on vs off) | unsupported on this guest (KVA shadow) | — | — |

The p99 outliers (≈62 ms) are host scheduling artefacts of the shared VM
host, not transport retries; medians are stable across runs (71–82 us). The
syscall-overhead line is only produced when InfinityHook installs; this
KVA-shadow guest does not permit that (see `docs/compat.md`), so `bench`
prints `syscall-overhead unsupported on this build`.

## What dominates the cost

- **Transport**: a synchronous kchannel call with ChaCha20 + HMAC-SHA256 and
  a worker wake-up; this is the floor for every operation (small payloads are
  transport bound, large payloads approach the copy bandwidth).
- **Physical reads**: one `MmCopyMemory` per 4 KiB page.
- **Physical writes**: one `MmMapIoSpaceEx`/`MmUnmapIoSpace` pair per page.
  Larger writes are slower per byte than reads; batch multiple pages in one
  CLI run to amortise the round trips.
- **Process memory**: four page-table reads (PML4/PDPT/PD/PT) per translation
  plus the physical copy. For sequential 4 KiB reads the page tables are
  re-walked for every call; batching ranges reduces the relative overhead.
- **Kernel virtual memory**: same walk against the current (system) CR3.

## Optimisation notes

- Run `bench` with `--iterations 2000` for stable p99 numbers; the VM is a
  shared host, so first runs are noisy.
- `DRVLITE_MAX_TRANSFER` is 0xFC00 (63 KiB). The SDK splits larger requests;
  the driver never accepts a transaction above that limit.
- `kc_call` timeouts default to 5 s; a timeout is reported as
  `DRVLITE_ERR_TIMEOUT`, never a hang.
