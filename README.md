# STM32 WebAssembly Orchestration + Runtime Benchmarks

This repository contains (1) a distributed **WebAssembly orchestration** stack for STM32 boards running Zephyr + WAMR, and (2) a set of **performance benchmarks** to compare multiple execution stacks (native C vs Wasm runtimes) on STM32F446RE and STM32F746ZG, plus host-side reference runs. 


## Contents

- **Orchestration system** (control plane + data plane):
  - Host CLI (`host.py`)
  - Gateway/orchestrator (`gateway.py`)
  - Device firmware agent (Zephyr + WAMR), supporting dynamic module load/start/stop/status
- **Benchmarks**:
  - GPIO toggle maximum frequency (MCU + oscilloscope)
  - 1024-point complex radix‑2 FFT benchmark (MCU + host)


## Hardware & setup

### Boards (MCU measurements)
| Board | MCU | Core frequency | Oscilloscope | Probe | Connections |
|------|-----|----------------|--------------|-------|------------|
| NUCLEO‑F446RE | STM32F446RE (Cortex‑M4F) | 180 MHz | Rigol MSO5104 | ×10 | tip → PA5, GND → GND |
| NUCLEO‑F746ZG | STM32F746ZG (Cortex‑M7) | 216 MHz | Rigol MSO5104 | ×10 | tip → PA5, GND → GND |

### Host (reference runs)
- Linux (WSL2): Ubuntu on WSL2, Intel Core i5‑9600K pinned to 3.7 GHz (Turbo Boost disabled)


## Prerequisites

### Firmware (Zephyr)
- Zephyr SDK + `west` (follow Zephyr getting-started)
- ARM toolchain (via Zephyr SDK)
- Board support for `nucleo_f446re` and `nucleo_f746zg`

### Gateway/host tooling
- Python 3.x
- `pyserial` (UART access)
- `clang` with `--target=wasm32-unknown-unknown`
- WABT (`wat2wasm`) if you build WAT examples
- `wamrc` (WAMR AOT compiler)
- `xxd` (if you generate C headers from wasm/aot artifacts)


## Orchestration system (STM32 + Zephyr + WAMR)

### Architecture
- **Host** (`host.py`): CLI client; sends high-level commands (`deploy`, `build-and-deploy`, `start`, `stop`, `status`) to the gateway over TCP/JSON and prints `e2e_latency_ms`.
- **Gateway** (`gateway.py`): central orchestrator; receives host requests via TCP/JSON, optionally compiles C→WASM and WASM→AOT, maps `device` → serial port, and speaks a line-based protocol to the firmware agent.
- **Device firmware agent** (Zephyr + WAMR): runs on each board; implements module slots (currently `MAX_MODULES = 2`) and executes WASM/AOT modules inside WAMR.


### Device slots & memory choices (current defaults)
- `MAX_MODULES = 2` (two concurrent module slots)
- WAMR pool allocator with a fixed global pool (example: `216 KiB`) for stable memory behavior

### Gateway ↔ device protocol
Line-based ASCII commands (one per line), optionally followed by raw binary payload for `LOAD`:

- **LOAD**
  ```text
  LOAD module_id=<id> size=<N> crc32=<hex> [replace=1] [replace_victim=<id>]
  ```
  Flow:
  1) device → `LOAD_READY ...`
  2) gateway → sends exactly `N` raw bytes (WASM or AOT)
  3) device → `LOAD_OK ...` or `LOAD_ERR ...`

- **START**
  ```text
  START module_id=<id> func=<exported_name> [args="a=1,b=2"]
  ```
  device replies `START_OK` and then `RESULT status=...`.

- **STOP**
  ```text
  STOP module_id=<id>
  ```
  cooperative stop request for long-running jobs (device replies `STOP_OK ...` and later a final `RESULT ...`).

- **STATUS**
  ```text
  STATUS
  ```
  device returns a single-line status such as:
  ```text
  STATUS_OK modules="..." low_stack="..." wamr_total=... wamr_free=... wamr_used=... wamr_highmark=...
  ```

### Replace semantics (important)

The goal is “one request” replace, without requiring a manual `undeploy` first:

- If `module_id` **already exists**, `LOAD` reloads that slot (update in place).
  - If `replace_victim` is also provided, the agent should ignore it and may append a warning to `LOAD_OK` (e.g., `warn=VICTIM_IGNORED`).
- If `module_id` is **new** and slots are full:
  - Without `replace_victim`: `LOAD_ERR code=FULL msg="NEED_VICTIM"`
  - With `replace_victim=<id>`: the agent force-stops that victim (abort) and reuses its slot.

> The gateway treats `replace_victim` as implying `replace=1`, so the device will always see `replace=1` when a victim is provided.



## Orchestration quickstart

### 1) Flash firmware on the board

Example (F446RE):
```bash
cd zephyrproject/firmware
west build . -d build_f446 -b nucleo_f446re --pristine
west flash --build-dir build_f446
```

Example (F746ZG):
```bash
cd zephyrproject/firmware
west build . -d build_f746 -b nucleo_f746zg --pristine
west flash --build-dir build_f746
```

### 2) Configure gateway device mapping
Edit DEVICE_ENDPOINTS in gateway.py, e.g.:
```python
DEVICE_ENDPOINTS = {
  "nucleo_f446": "COM4",
  "nucleo_f746": "COM7",
}
```

### 3) Run gateway
```bash
python gateway.py --port 9000
```

### 4) Use host CLI

Status:
```bash
python host.py --device nucleo_f746 status
```

Build + deploy a C module as AOT:
```bash
python host.py --device nucleo_f746 \
  build-and-deploy --module-id toggle1 \
  --source wasm/c/toggle_forever.c \
  --mode aot
```

Start and wait result:
```bash
python host.py --device nucleo_f746 \
  start --module-id math_ops --func-name add --func-args "a=10,b=15" --wait-result
```

Replace victim when slots are full:
```bash
python host.py --device nucleo_f746 \
  build-and-deploy --module-id toggle3 \
  --source wasm/c/toggle_forever.c --mode aot \
  --replace-victim toggle2
```

## Benchmarks

### Goals
- **Toggle benchmark**: measure the maximum GPIO write/toggle frequency on the MCU to quantify runtime overhead. 
- **FFT benchmark**: measure performance of an in-place complex radix‑2 FFT (N=1024), using identical portable C code across native execution, Wasm interpreters, and WAMR AOT. 

### Stacks compared
- Bare-metal C
- Bare-metal + Wasm3
- FreeRTOS C
- FreeRTOS + Wasm3
- Zephyr C 
- Zephyr + Wasm3 
- Zephyr + WAMR (Interpreter) 
- Zephyr + WAMR (AOT) 
- Host Linux: native C / Wasm3 / WAMR (interpreter + AOT) 

Detailed result tables and memory footprint tables are stored in `benchmarks/results/`.

### Toolchain notes

WASM build (example):
```bash
clang --target=wasm32-unknown-unknown -O3 -nostdlib \
  -Wl,--no-entry \
  -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
  -Wl,--stack-first -Wl,-z,stack-size=2048 \
  toggle_forever.c -o toggle_forever.wasm
```

AOT build for Cortex‑M4/M7 class MCUs (example):
```bash
wamrc --target=thumbv7em --target-abi=gnu --cpu=cortex-m4 \
  -o toggle_forever.aot toggle_forever.wasm
```
