# Architecture — DistributedGPU

## System overview

```
┌──────────────────────────────────────────┐    10 Gbps Ethernet
│               Laptop A (Host)            │◄──────────────────────►│  Laptop B (Worker)  │
│                                          │                         │                     │
│  ┌─────────────┐    ┌────────────────┐   │                         │  ┌───────────────┐  │
│  │ host app    │───►│  TaskClient    │───┼── TCP SubmitTask ──────►│  │  TaskServer   │  │
│  │ (RTX 3050)  │    │  (networking)  │   │                         │  │  (networking) │  │
│  └─────────────┘    └───────┬────────┘   │                         │  └──────┬────────┘  │
│         │                  │ TaskResult  │◄── TCP TaskResult ──────┼─────────┘           │
│  ┌──────▼──────┐           │             │                         │  ┌───────────────┐  │
│  │ frame       │◄──────────┘             │                         │  │  GpuExecutor  │  │
│  │ compositor  │                         │                         │  │  (GTX 1650Ti) │  │
│  └─────────────┘                         │                         │  └───────────────┘  │
└──────────────────────────────────────────┘                         └─────────────────────┘
```

## Module dependency graph

```
protocol  ◄── networking ◄── host app
    ▲              ▲         worker app
    │              │
telemetry ◄── d3d12 ◄────── host app
    ▲                        worker app
    │                        benchmark app
task_runtime ◄──────────── host app
```

`protocol` and `networking` have **no DirectX 12 dependency**.  
This is intentional — network correctness can be tested without a GPU.

## Data flow (M4)

```
Host renders scene
    │
    ▼
[RTX 3050] GPU → readback → CPU memory
    │
    ▼ (TCP, SubmitTask)
[GTX 1650 Ti] upload → GPU compute (blur/edge) → readback → CPU memory
    │
    ▼ (TCP, TaskResult)
[RTX 3050] upload result → composite → display
```

## Threading model

### Host process

| Thread         | Responsibility                                                  |
|----------------|-----------------------------------------------------------------|
| Main           | Render loop, frame composition                                  |
| TaskQueue × N  | Dispatch local or remote tasks from the async queue             |
| Network recv   | Owned internally by `TcpConnection::receiveMessage` (blocking)  |

### Worker process

| Thread         | Responsibility                                                  |
|----------------|-----------------------------------------------------------------|
| Accept         | `TaskServer` accept loop; spawns a handler thread per client    |
| Handler × M    | One per connected client; receives tasks, dispatches to GPU     |
| GPU            | Single-GPU critical section inside `GpuExecutor` (mutex-guarded)|

## Resource lifecycle

### GPU buffers (M5 pool)

```
GpuExecutor::Impl::bufCache
  ├── upload    (BufferUsage::Upload)   — CPU-writable staging
  ├── gpuInput  (BufferUsage::Default)  — shader SRV source
  ├── gpuOutput (BufferUsage::Default)  — shader UAV target
  └── readback  (BufferUsage::Readback) — CPU-readable result
```

The buffer set is reused across tasks of the same dimensions.  
A dimension change triggers re-allocation (one stall per resize).

## Circuit breaker (M5)

The `CircuitBreaker` in `task_runtime` uses a three-state machine:

```
Closed ──(≥ N failures)──► Open ──(cooldown elapsed)──► HalfOpen
  ▲                                                          │
  └────────────────(probe succeeds)─────────────────────────┘
```

When `Open`, all remote submissions are redirected to the local executor.

## Extending the task registry

To add a new task type:

1. Add a value to `protocol::TaskType` enum.
2. Add an HLSL shader to `shaders/`.
3. Embed or load the shader in `GpuExecutor::init()`.
4. Add a dispatch branch in `GpuExecutor::execute()`.
5. Update `TaskRequest::validate()` to accept the new type.
6. Bump `PROTO_VERSION` if the wire format changes.

**Do not** allow arbitrary shader upload — the task registry approach keeps the attack surface minimal and avoids remote code execution risks.
