# STM32 WASM Runtime Benchmarks

This repository contains a set of experiments to compare the performance of different execution stacks on **STM32F446RE (180 MHz)** and **STM32F746ZG (216 MHz)**, and on host platform (Linux), measuring:
1. The maximum GPIO switching (toggle) frequency using an oscilloscope (MCU only).
2. Performance on a real numerical algorithm (complex radix-2 FFT) both on MCU and on host (Linux).

The goal is to quantify the overhead introduced by:

- Bare-metal C
- Bare-metal + WebAssembly (Wasm3)
- FreeRTOS
- FreeRTOS + WebAssembly (Wasm3)
- Zephyr
- Zephyr + WebAssembly (Wasm3)
- Zephyr + WAMR (Interpreter)
- Zephyr + WAMR (AOT)
- Linux host (WSL2) + native C / wasm3 / WAMR (interpreter + AOT)

The repository provides:
- Full source code for all tested stacks
- Portable C FFT code (radix-2 Cooley–Tukey)
- WASM and AOT bytecode (WAMR)
- Instructions and commands to generate `.h` headers from WASM/AOT files
- Real measurement methodology
- Experimental results for toggling and FFT

<br>

## Hardware used

| Board | MCU | Core frequency | Oscilloscope | Probe | Connections |
|--------|-----|----------------|--------------|-------|--------------|
| **Nucleo-F446RE** | STM32F446RET6 (Cortex-M4F) | **180 MHz** | Rigol MSO5104 | ×10 | tip → PA5, GND → GND |
| **Nucleo-F746ZG** | STM32F746ZGT6 (Cortex-M7) | **216 MHz** | Rigol MSO5104 | ×10 | tip → PA5, GND → GND |

For host test:

- Linux (WSL2): Ubuntu on WSL2, Intel Core i5‑9600K CPU pinned to 3.7 GHz (Turbo Boost disabled).


<br>

## Benchmark goal

1. **Toggle benchmark**  
   Measure the maximum GPIO write frequency (BSRR) in an infinite loop, to evaluate runtime overhead.

2. **FFT benchmark**  
   Measure performance of an in-place complex radix-2 FFT (N=1024):
   - Implemented in pure portable C
   - Identical for native tests and Wasm runtimes
   - Compiled as:
     - Native ARM via GCC (STM32CubeIDE)
     - Wasm interpreter via clang → wasm32
     - WAMR AOT via `wamrc --target=thumbv7em`
     - Host Linux via native toolchain + clang → wasm32 + WAMR AOT (x86‑64)

The FFT includes bit-reversal, 10 stages, precomputed twiddle factors, and the full Cooley–Tukey DIT flow.

<br>

## Memory usage (Flash/RAM)

In embedded systems, Flash and RAM are critical constraints. Below is an analysis of the memory footprint of the runtimes on Zephyr, distinguishing between static (compile-time) and dynamic (run-time) usage.

### Methodology

- **Flash (ROM)**: total firmware image size (`text + rodata + data`), including RTOS kernel, Wasm runtime, and application.
- **Static RAM**: global data and statically allocated buffers (`data + bss`).
- **Dynamic RAM**: heap and stack required at runtime.
- **Total RAM**: effective sum of the resources used.

**Note**: the `*` symbol indicates configurations where part of the “dynamic” memory is accounted as static (e.g., WAMR static pool, or Zephyr system heap counted statically).

### Toggle – Memory footprint (F446RE)

| Configuration | Flash (ROM) | Static RAM | Dynamic RAM | Total RAM |
|---|---:|---:|---:|---:|
| Zephyr + Wasm3 | 77.80 KiB | 4.69 KiB | ~12.00 KiB¹ | ~16.69 KiB |
| Zephyr + WAMR (Interp) | 85.16 KiB | 77.81 KiB * | Included * | ~77.81 KiB |
| Zephyr + WAMR (AOT) | 78.52 KiB | 80.31 KiB * | Included * | ~80.31 KiB |

<small>¹ Estimate: Wasm heap (8 KiB) + thread stack (4 KiB).</small>  
<small>* Includes a statically preallocated buffer (e.g., pool) accounted in static RAM.</small>

### Toggle – Memory footprint (F746ZG)

| Configuration | Flash (ROM) | Static RAM | Dynamic RAM | Total RAM |
|---|---:|---:|---:|---:|
| Zephyr + Wasm3 | 78.36 KiB | 4.75 KiB | ~12.00 KiB¹ | ~16.75 KiB |
| Zephyr + WAMR (Interp) | 85.64 KiB | 77.87 KiB * | Included * | ~77.87 KiB |
| Zephyr + WAMR (AOT) | 78.98 KiB | 80.37 KiB * | Included * | ~80.37 KiB |

<small>¹ Estimate: Wasm heap (8 KiB) + thread stack (4 KiB).</small>  
<small>* Includes a statically preallocated buffer (e.g., pool) accounted in static RAM.</small>

### FFT – Memory footprint (F446RE, N=1024)

In this configuration, WAMR uses the system allocator (`malloc`) instead of a static buffer.

| Configuration | Flash (ROM) | Static RAM | Dynamic RAM | Total RAM |
|---|---:|---:|---:|---:|
| Zephyr + Wasm3 | 98.19 KiB | 37.13 KiB * | Included * | ~37.13 KiB |
| Zephyr + WAMR (Interp) | 106.02 KiB | 19.38 KiB | 16.00 KiB | ~35.38 KiB |
| Zephyr + WAMR (AOT) | 99.50 KiB | 22.13 KiB | 16.00 KiB | ~38.13 KiB |

<small>* Wasm3: the dynamic heap is counted in static RAM by Zephyr (e.g., `CONFIG_HEAP_MEM_POOL_SIZE`).</small>

### FFT – Memory footprint (F746ZG, N=1024)

| Configuration | Flash (ROM) | Static RAM | Dynamic RAM | Total RAM |
|---|---:|---:|---:|---:|
| Zephyr + Wasm3 | 98.68 KiB | 37.25 KiB * | Included * | ~37.25 KiB |
| Zephyr + WAMR (Interp) | 106.81 KiB | 19.37 KiB | 16.00 KiB | ~35.37 KiB |
| Zephyr + WAMR (AOT) | 99.99 KiB | 22.25 KiB | 16.00 KiB | ~38.25 KiB |

<small>* Wasm3: the dynamic heap is counted in static RAM by Zephyr (e.g., `CONFIG_HEAP_MEM_POOL_SIZE`).</small>

<br>

## WebAssembly code used

### Minimal WAT (toggle)

```wat
(module
  (import "env" "gpio_toggle" (func $gpio_toggle))
  (func (export "toggle_forever")
    (loop $L
      call $gpio_toggle
      br $L)))
```

### Compile to `.wasm`

```bash
wat2wasm toggle.wat -o toggle.wasm
xxd -i toggle.wasm > toggle.wasm.h
```

### AOT (WAMR)

```bash
wamrc --target=thumbv7em --target-abi=eabi \
  --opt-level=3 --size-level=0 \
  --bounds-checks=0 --stack-bounds-checks=0 \
  -o toggle.aot toggle.wasm
xxd -i toggle.aot > toggle_aot.h
```
<br>

## FFT: implementation used

The FFT used is:
- radix-2 Cooley–Tukey DIT
- complex
- interleaved in-place buffer \[re, im, re, im, ...\]
- N = 1024
- 10 full stages
- precomputed twiddles (no dependency on `math.h`)
- identical across native ARM, Wasm3, WAMR (interpreter + AOT), and host Linux.

The Wasm module exports:
```c
__attribute__((export_name("fft_init")))
void fft_init(void);

__attribute__((export_name("fft_bench")))
void fft_bench(int iterations);
```

Compilation:
```bash
clang --target=wasm32-unknown-unknown \
  -O3 -nostdlib \
  -Wl,--no-entry \
  -Wl,-z,stack-size=16384 \
  -o fft_bench.wasm \
  fft_bench_wasm.c

xxd -i fft_bench.wasm > fft_bench.wasm.h
```

AOT for MCU (ARM Thumb-2):
```bash
wamrc --target=thumbv7em --target-abi=eabi -o fft_bench.aot fft_bench.wasm

xxd -i fft_bench.aot > fft_bench_aot.h
```

AOT for x86_64 Linux:
```bash
wamrc --target=x86_64 \
  --opt-level=3 --size-level=0 \
  --bounds-checks=0 --stack-bounds-checks=0 \
  -o fft_bench.aot fft_bench.wasm

xxd -i fft_bench.aot > fft_bench_aot.h
```

<br>

## Experimental results – Toggle Benchmark (F446RE, ~180 MHz core)

| Stack | Runtime / Mode | Measured frequency | Slowdown vs bare-metal C |
|------------------|--------------------|--------------------|--------------------------|
| Bare-metal | Native C | **~36.4 MHz** | **1.00×** |
| FreeRTOS | Native C | **~36.4 MHz** | **1.00×** |
| Zephyr | Native C | **~36 MHz** | **1.01×** |
| Bare-metal | wasm3 (interpreter) | **~502 kHz** | **72.5×** |
| FreeRTOS | wasm3 (interpreter) | **~564 kHz** | **64.5×** |
| Zephyr | wasm3 (interpreter) | **~523 kHz** | **69.6×** |
| Zephyr | WAMR (interpreter) | **~207 kHz** | **175.8×** |
| Zephyr | WAMR (AOT) | **~346 kHz** | **105.2×** |

**Toggle F446RE notes:**
- The three “native” cases (Bare-metal, FreeRTOS, Zephyr) align at ~36 MHz, essentially at the limit of how fast the core can toggle by writing to BSRR.
- Kernel overhead (FreeRTOS/Zephyr) is negligible compared to the tight toggle loop.
- Wasm3 introduces a slowdown factor of about **70×** compared to native.
- In the current setup, the WAMR interpreter is slower than Wasm3.
- WAMR AOT significantly improves over its interpreter, but remains slower than Wasm3 with the current configuration.
- Anomaly: wasm3 appears faster on an RTOS than on bare-metal (needs careful investigation).
- WAMR is not available bare-metal on these platforms; it is available for Zephyr on Nucleo, and for FreeRTOS only in the ESP-IDF ecosystem.
- AOT means compiling Wasm into native code; only the Wasm runtime remains to manage execution, with no interpretation. In WAMR, AOT files are generated with `wamrc`. 

<br>

## Experimental results – Toggle Benchmark (F746ZG, ~216 MHz core)

| Stack | Runtime / Mode | Measured frequency | Slowdown vs bare-metal C |
|------------------|--------------------|--------------------|----------------------|
| Bare-metal | Native C | **~110 MHz** | **1.00×** |
| FreeRTOS | Native C | **~110 MHz** | **1.00×** |
| Zephyr | Native C | **~108 MHz** | **1.02×** |
| Bare-metal | wasm3 (interpreter) | **~908 kHz** | **121.1×** |
| FreeRTOS | wasm3 (interpreter) | **~907 kHz** | **121.3×** |
| Zephyr | wasm3 (interpreter) | **~1.07 MHz** | **102.8×** |
| Zephyr | WAMR (interpreter) | **~334 kHz** | **329.3×** |
| Zephyr | WAMR (AOT) | **~467 kHz** | **235.6×** |

**Toggle F746ZG notes:**
- Native Cortex-M7 is ~3× faster than the F4 (110 MHz vs 36 MHz).
- Kernel overhead (FreeRTOS/Zephyr) is negligible on the tight loop.
- Wasm3 on F7 is ~1.8× faster than on F4 (908 kHz vs 502 kHz).
- WAMR AOT remains slower than Wasm3 (~2.3× relative slowdown).

<br>

## FFT Benchmark (F446RE, N = 1024, 100 iterations)

Metric: average cycles per 1024-point FFT, obtained by counting total cycles over 100 consecutive runs and dividing by the number of iterations.

| Environment | Runtime / Mode | Average cycles per FFT | Slowdown vs bare-metal C |
|--------------|--------------------------|-------------------:|-------------------------:|
| Bare-metal | Native C | **219 022** | **1.00×** |
| FreeRTOS | Native C | **219 009** | **1.00×** |
| Zephyr | Native C | **228 196** | **1.04×** |
| Bare-metal | wasm3 (interpreter) | **17 566 414** | **80.20×** |
| FreeRTOS | wasm3 (interpreter) | **16 103 862** | **73.53×** |
| Zephyr | wasm3 (interpreter) | **14 975 767** | **68.38×** |
| Zephyr | WAMR (interpreter) | **13 567 746** | **61.95×** |
| Zephyr | WAMR (AOT) | **3 341 702** | **15.26×** |

**FFT F446RE notes:**
- Wasm interpreters show a 60–80× slowdown.
- WAMR AOT reduces it to ~15×.
- FreeRTOS/Zephyr overhead is effectively zero.

<br>

## FFT Benchmark (F746ZG, N = 1024, 100 iterations)

Metric: average cycles per 1024-point FFT, obtained by counting total cycles over 100 consecutive runs and dividing by the number of iterations.

| Environment | Runtime / Mode | Average cycles per FFT | Slowdown vs bare-metal C |
|--------------|--------------------------|-------------------:|-------------------------:|
| Bare-metal | Native C | **124 108** | **1.00×** |
| FreeRTOS | Native C | **124 562** | **1.00×** |
| Zephyr | Native C | **171 412** | **1.38×** |
| Bare-metal | wasm3 (interpreter) | **15 932 553** | **128.4×** |
| FreeRTOS | wasm3 (interpreter) | **13 962 748** | **112.5×** |
| Zephyr | wasm3 (interpreter) | **14 270 967** | **115.0×** |
| Zephyr | WAMR (interpreter) | **8 759 838** | **70.6×** |
| Zephyr | WAMR (AOT) | **2 542 771** | **20.5×** |

**FFT F746ZG notes:**
- Native C Bare-metal/FreeRTOS is ~1.8× faster than F4 (124k vs 219k cycles).
- Zephyr native C has ~38% overhead vs bare-metal.
- Among Wasm interpreters, WAMR is the fastest on F7 (~8.8M cycles, ~70× slowdown), followed by wasm3 (~14–16M cycles).
- WAMR AOT further reduces the gap, down to ~20× slowdown vs native C.

<br>

## FFT Benchmark – Linux (WSL2, N = 1024, 100 iterations)

Metric: equivalent average cycles per 1024-point FFT, computed from nanosecond measurements on Ubuntu in WSL2, with an Intel Core i5‑9600K pinned to 3.7 GHz (Turbo Boost disabled).

| Environment | Runtime / Mode | Average cycles per FFT | Slowdown vs native C |
|--------------|----------------------|--------------------:|----------------------:|
| Linux (WSL2) | Native C | 28 875 | 1.0× |
| Linux (WSL2) | wasm3 (interpreter) | 784 509 | 27.2× |
| Linux (WSL2) | WAMR (interpreter) | 1 448 892 | 50.2× |
| Linux (WSL2) | WAMR (AOT) | 50 577 | 1.75× |

**FFT Linux (WSL2) notes:**
- Native C remains the fastest baseline at ~28.9k cycles per FFT.
- wasm3 introduces ~27× slowdown vs native C, while the WAMR interpreter is ~50× slower.
- WAMR AOT stays close to native (~1.75×), consistent with AOT delivering near-native performance on hosts. 


<br>

## Toolchain used

- Bare-metal/FreeRTOS: STM32CubeIDE
- Zephyr: Zephyr SDK + west
- WASM:
  - WABT (`wat2wasm`)
  - clang (wasm32 compilation)
  - xxd
  - Wasm3
  - WAMR (Interpreter + AOT)
  - `wamrc` (AOT → ARM Thumb2 / x86‑64 depending on the target)

<br>

## Build and flash (Bare-metal and FreeRTOS)

Projects can be imported directly into STM32CubeIDE.

<br>

## Build and flash in Zephyr

Install Zephyr SDK + west (official getting-started guidelines):
https://docs.zephyrproject.org/latest/develop/getting_started/index.html#getting-started

**F446RE**:
```bash
cd zephyrproject/z_native_f4
west build . -b nucleo_f446re --pristine
west flash
```

**F746ZG:**
```bash
cd zephyrproject/z_native_f7
west build . -b nucleo_f746zg --pristine
west flash


```
