# DistributedGPU

A C++ system that lets two Windows laptops cooperate at the GPU level — starting with a working DirectX 12 distributed-rendering prototype.

| Laptop | GPU | Role |
|---|---|---|
| **A (Host)** | RTX 3050 · 4 GB | Primary rendering, compositing, display |
| **B (Worker)** | GTX 1650 Ti · 4 GB | Remote compute tasks, returns results |

## GitHub Workflow & Cross-Device Releases

All source code and precompiled builds are managed through the private GitHub repository:  
**[`https://github.com/CAsishSrinivas/DistributedGPU`](https://github.com/CAsishSrinivas/DistributedGPU)**

No files or binaries need to be manually transferred between laptops.

### 1. One-Click Setup on Laptop B (Worker)

On **Laptop B** (GTX 1650 Ti), simply run this single PowerShell command to download the latest release, extract it, and start the worker daemon:

```powershell
# From the cloned repository root:
.\scripts\update_laptop_b.ps1 -StartWorker
```
Or download [`DistributedGPU-windows-x64-v1.0.0.zip`](https://github.com/CAsishSrinivas/DistributedGPU/releases/tag/v1.0.0) directly from GitHub Releases.

### 2. Publishing New Releases from Laptop A (Host)

Whenever changes are validated on **Laptop A**, publish a new version directly to GitHub Releases in one command:

```powershell
.\scripts\publish_release.ps1 -Version "1.0.1"
```
This automatically:
1. Compiles all targets in `Release` mode with MSVC.
2. Runs the full CTest regression test suite.
3. Packages the binaries, shaders, proxy DLLs, and default configuration.
4. Creates a new GitHub Release and uploads the zip asset via the GitHub REST API.

---

## Quick start

### Prerequisites

- Windows 10/11 (both laptops)
- Visual Studio 2022 with **Desktop development with C++** workload
- Windows SDK 10.0.20348.0 or later (provides DirectX 12 headers)
- NVIDIA drivers ≥ 528.x on both machines
- 10 Gbps / 1 Gbps Ethernet cable or high-speed Wi-Fi between the laptops

### Build from Source

```powershell
# Clone the repository
git clone https://github.com/CAsishSrinivas/DistributedGPU.git
cd DistributedGPU

# Build Release binaries
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Run unit and integration tests
ctest --test-dir build -C Release --output-on-failure
```

Binaries land in `build/bin/Release/`:
- `benchmark.exe`    — M0/M1 local GPU validation
- `worker.exe`       — Distributed worker daemon (run on **Laptop B**)
- `host.exe`         — Distributed coordinator & Stage 5 benchmark (run on **Laptop A**)
- `dxgi.dll`         — DirectX 12 live game proxy hook (e.g. for Elden Ring)
- `game_adapter.exe` — Standalone game integration test harness

### First run order

1. **Laptop B** — start worker: `.\build\bin\Release\worker.exe 7777`
2. **Laptop A** — run host: `.\build\bin\Release\host.exe 192.168.1.14 7777`

If the worker is unreachable, `host.exe` falls back to local execution.

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
