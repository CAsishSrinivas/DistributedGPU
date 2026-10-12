# Benchmark Plan — M1 and M4

## Purpose

This document defines the repeatable measurement protocol for:

- **M1** — Establishing local GPU baselines on each machine independently.
- **M4** — Comparing local vs. remote vs. async-remote execution.

All measurements are recorded by the `Telemetry` singleton and exported to CSV.  
Analysis scripts should treat the CSV as the ground truth.

## M1 — Local baseline

Run `benchmark.exe` on **each machine separately** and record the output CSV.

### Test matrix

| Configuration | Width | Height | Warm-up | Measured runs |
|---------------|-------|--------|---------|---------------|
| blur          | 256   | 256    | 5       | 50            |
| blur          | 512   | 512    | 5       | 50            |
| blur          | 1024  | 1024   | 3       | 30            |
| blur          | 1920  | 1080   | 2       | 20            |
| edge_detect   | 256   | 256    | 5       | 50            |
| edge_detect   | 1920  | 1080   | 2       | 20            |

### Metrics recorded per run

| Metric       | Source         | Description                         |
|--------------|----------------|-------------------------------------|
| `uploadUs`   | GPU executor   | CPU → GPU transfer time             |
| `gpuExecUs`  | GpuTimer       | Fence-gated shader execution time   |
| `readbackUs` | GPU executor   | GPU → CPU transfer time             |
| `totalUs`    | Wall clock     | End-to-end frame time               |

### Acceptance criteria (M1)

1. Both machines complete the same test matrix without GPU errors.
2. Two identical runs produce outputs with mean absolute error ≤ 5 / 255.
3. RTX 3050 `gpuExecUs` ≤ GTX 1650 Ti for the same task (expected).

---

## M4 — Distributed rendering experiment

Run `host.exe <worker-ip>` with the worker running on Laptop B.

### Three configurations per resolution

| Mode          | Description                                                    |
|---------------|----------------------------------------------------------------|
| `local`       | RTX 3050 executes the filter locally                           |
| `remote_sync` | RTX 3050 blocks waiting for GTX 1650 Ti result                 |
| `remote_async`| RTX 3050 submits the task and does independent work meanwhile  |

### Additional metrics for remote modes

| Metric              | Description                                              |
|---------------------|----------------------------------------------------------|
| `networkSendUs`     | Time to transmit input buffer from host to worker        |
| `networkReturnUs`   | Time to receive output buffer from worker to host        |
| `t_total`           | Critical-path time (host perspective, wall clock)        |

### Decision criterion

Remote execution is worth pursuing if, for any (resolution × task) combination:

```
t_worker_gpu + t_network_send + t_network_return < t_local_gpu
```

Or, in async mode:

```
max(t_host_independent_work, t_remote_total) < t_local_total
```

### What to do with results

1. Export CSVs from both host and worker.
2. Merge on `taskId` column.
3. Plot `totalUs` distributions for each configuration per resolution.
4. Identify crossover resolution (if any) where remote > local.
5. Document findings in `docs/game-compatibility.md` under "Offload suitability".

---

## Notes

- Run tests with no other GPU workloads active (close browsers, games, etc.).
- Power-cap both laptops to AC power mode.
- Re-run the baseline after driver updates.
- The `benchmark.exe` binary re-uses the same HLSL shaders embedded in `gpu_executor.cpp`.
  If you edit the shaders, rebuild before re-running benchmarks.
