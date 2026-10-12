# Wire Protocol — DistributedGPU v1

## Byte order

All multi-byte integers on the wire are **big-endian** (network byte order).  
Both ends call the `hton`/`ntoh` family before sending and after receiving.

## Message frame

Every message consists of a **fixed 24-byte header** followed by a variable-length payload.

```
Offset  Size  Field          Type      Description
──────  ────  ─────────────  ────────  ────────────────────────────────────────
0       4     magic          uint32    Must be 0x44475055 ("DGPU")
4       2     version        uint16    Must be 0x0001 (v1)
6       2     messageType    uint16    See table below
8       8     taskId         uint64    Correlation ID (0 for control messages)
16      4     payloadSize    uint32    Byte count of the payload that follows
20      4     status         uint32    TaskStatus (0 = Ok)
```

`sizeof(MessageHeader) == 24` is statically asserted at compile time.

## Message types

| Value  | Name          | Direction        | Payload                    |
|--------|---------------|------------------|----------------------------|
| 0x0001 | Hello         | client → server  | HelloPayload               |
| 0x0002 | HelloAck      | server → client  | _(empty)_                  |
| 0x0010 | SubmitTask    | client → server  | TaskRequest                |
| 0x0011 | TaskAccepted  | server → client  | _(empty)_                  |
| 0x0012 | TaskResult    | server → client  | TaskResult                 |
| 0x0013 | TaskError     | server → client  | TaskResult (status ≠ Ok)   |
| 0x0020 | Ping          | either           | _(empty)_                  |
| 0x0021 | Pong          | either           | _(empty)_                  |
| 0x00FF | Shutdown      | client → server  | _(empty)_                  |

## Task types (v1)

| Value | Name          | Input        | Output       |
|-------|---------------|--------------|--------------|
| 1     | Blur          | RGBA8 buffer | RGBA8 buffer |
| 2     | EdgeDetection | RGBA8 buffer | RGBA8 buffer |

Arbitrary shader upload or code execution is **not supported** in v1.

## HelloPayload

```
[uint32 hostname_len][hostname UTF-8 bytes]
[uint32 gpuName_len ][gpuName  UTF-8 bytes]
[uint32 vramMb]
```

## TaskRequest payload

```
[uint64 taskId]
[uint32 type]         // TaskType enum
[uint32 width]
[uint32 height]
[uint32 inputSize]    // Must equal width * height * 4
[byte*  inputSize]    // RGBA8 pixel data, row-major
```

Validation rules (server rejects if any fail):
- `width` and `height` must be in `[1, 4096]`
- `inputSize` must equal `width * height * 4`
- `type` must be a registered TaskType value

## TaskResult payload

```
[uint64 taskId]
[uint32 status]       // TaskStatus enum
[uint64 uploadUs]     // Worker GPU upload time (microseconds)
[uint64 gpuUs]        // Worker GPU execution time (microseconds)
[uint64 readbackUs]   // Worker GPU readback time (microseconds)
[uint64 totalUs]      // Worker total processing time (microseconds)
[uint32 outputSize]
[byte*  outputSize]   // RGBA8 pixel data, same dimensions as input
```

## Task status codes

| Value | Name            | Meaning                                      |
|-------|-----------------|----------------------------------------------|
| 0     | Ok              | Success                                      |
| 1     | InvalidRequest  | Bad dimensions, payload size, or task type   |
| 2     | UnsupportedTask | TaskType not in registry                     |
| 3     | GpuError        | DirectX 12 resource or execution failure     |
| 4     | Timeout         | Worker did not complete in time              |
| 5     | InternalError   | Unexpected worker error                      |

## Error handling rules

- A receiver that gets a message with `magic ≠ 0x44475055` must close the connection.
- A receiver that gets `payloadSize > MAX_PAYLOAD (64 MiB)` must close the connection.
- A receiver that gets a truncated payload (EOF before `payloadSize` bytes) must close the connection.
- The server must respond to every `SubmitTask` with exactly one `TaskResult` or `TaskError`.
- The server must survive malformed messages without crashing (validated by the networking test).
