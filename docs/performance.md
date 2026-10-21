# Performance

The numbers below are produced by `drvlite-cli run ... -- bench` inside the
`bvmenv` test VM (Windows 11 x64, 4 vCPU, kchannel `KC_MODE_MAPPED`, region
size 0x40000, 200 iterations per operation, one 4 KiB payload per call).
Measurements are wall-clock per `kc_call` round trip including the kchannel
transport, so they describe the SDK-facing cost, not just the memory copy.

```powershell
# inside the guest, after mapping the driver
drvlite-cli.exe run --image C:\drvlite\drvlite.sys `
    --map-cmd "C:\drvlite\drvlite-mapper-cli.exe --image {image} --bootstrap {bootstrap}" `
    -- bench --iterations 200
```

The command prints, for each operation, the median and p99 latency and the
effective throughput for a 4 KiB transfer. `docs/evidence/<date>-drv-lite/bench.txt`
keeps the raw output of every e2e run.

## Reference table (filled by the e2e run)

| Operation | Median | p99 | Throughput |
|---|---|---|---|
| physical read 4 KiB | _pending VM run_ | | |
| physical write 4 KiB | _pending VM run_ | | |
| kernel virtual read 4 KiB | _pending VM run_ | | |
| process read 4 KiB (CR3 walk) | _pending VM run_ | | |
| process write 4 KiB | _pending VM run_ | | |

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
