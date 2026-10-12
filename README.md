# DistributedGPU

A C++ system that lets two Windows laptops cooperate at the GPU level — starting with a working DirectX 12 distributed-rendering prototype.

| Laptop | GPU | Role |
|---|---|---|
| **A (Host)** | RTX 3050 · 4 GB | Primary rendering, compositing, display |
| **B (Worker)** | GTX 1650 Ti · 4 GB | Remote compute tasks, returns results |

## Quick start

### Prerequisites

- Windows 10/11 (both laptops)
- Visual Studio 2022 with **Desktop development with C++** workload
- Windows SDK 10.0.20348.0 or later (provides DirectX 12 headers)
- NVIDIA drivers ≥ 528.x on both machines
- 10 Gbps Ethernet cable between the laptops (or at least 1 Gbps for early testing)

### Build

```powershell
# Clone / open the project root
cd DistributedGPU

# Configure (Debug — recommended for early milestones)
cmake --preset windows-debug

# Build all targets
cmake --build build/debug

# Run unit tests
ctest --preset default --output-on-failure
```

Binaries land in `build/debug/bin/`:
- `benchmark.exe` — M0/M1 local GPU validation (run this first)
- `worker.exe`    — run on **Laptop B**
- `host.exe`      — run on **Laptop A** after worker is ready

### First run order

1. **Laptop B** — open a port: `worker.exe 7777`
2. **Laptop A** — `host.exe 192.168.1.101 7777` (replace with Laptop B's IP)

If the worker is unreachable, `host.exe` falls back to a local-only benchmark.

## Development roadmap

| Milestone | Description | Target |
|-----------|-------------|--------|
| **M0** | Environment and network validation | ½–1 day |
| **M1** | Local DirectX 12 benchmark (`benchmark.exe`) | 1–2 days |
| **M2** | Host–worker networking (TCP framing, handshake) | 2–3 days |
| **M3** | Remote GPU compute (execute shader on GTX 1650 Ti) | 2–4 days |
| **M4** | Distributed rendering experiment (timing matrix) | 3–5 days |
| **M5** | Async scheduling, resource pooling, circuit breaker | 3–5 days |
| **M6** | Commercial-game feasibility assessment | Research |
| **M7** | Game-specific integration prototype | Variable |

## Repository layout

```
DistributedGPU/
├── CMakeLists.txt        # Root build system
├── CMakePresets.json     # Debug / Release / Ninja presets
├── apps/
│   ├── host/             # Laptop A entry point
│   ├── worker/           # Laptop B entry point
│   └── benchmark/        # Standalone M0/M1 validator
├── libs/
│   ├── protocol/         # Wire format, serialisation
│   ├── networking/       # Winsock TCP, framing
│   ├── d3d12/            # Device, buffers, pipelines, timers, GpuExecutor
│   ├── task_runtime/     # Async queue, scheduler, circuit breaker
│   └── telemetry/        # Timing, stats, CSV export
├── shaders/
│   ├── blur.hlsl         # 5-tap Gaussian (separable)
│   └── edge_detect.hlsl  # Sobel operator
├── tests/
│   ├── protocol_tests/   # Serialisation round-trips and validation
│   ├── networking_tests/ # Localhost ping-pong and large payload
│   ├── gpu_tests/        # Output correctness and determinism
│   └── benchmark_tests/  # Telemetry statistics and CSV
└── docs/
    ├── architecture.md
    ├── protocol.md
    ├── benchmark-plan.md
    └── game-compatibility.md
```

## Acceptance criteria checklists

Run `ctest --preset default` to verify M0–M2 automatically.  
M3–M4 require both machines; run `benchmark.exe` then `host.exe + worker.exe`.

See [`docs/benchmark-plan.md`](docs/benchmark-plan.md) for the full measurement matrix.
