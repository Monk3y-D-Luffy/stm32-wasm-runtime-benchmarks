# STM32 WASM Runtime Benchmarks

Questo repository contiene una serie di esperimenti per confrontare le prestazioni di diversi stack di esecuzione su **STM32F446RE (180 MHz)** e **STM32F746ZG (216 MHz)**, e su piattaforme host (Linux/Android), misurando:
1. La frequenza massima di commutazione GPIO (toggle) tramite oscilloscopio (solo MCU).
2. Le prestazioni su un algoritmo numerico reale (FFT radix-2 complessa) sia su MCU che su host (Linux, Android).

L'obiettivo è quantificare l'overhead introdotto da:

- Bare-metal C  
- Bare-metal + WebAssembly (Wasm3)  
- FreeRTOS  
- FreeRTOS + WebAssembly (Wasm3)  
- Zephyr  
- Zephyr + WebAssembly (Wasm3)  
- Zephyr + WAMR (Interprete)  
- Zephyr + WAMR (AOT)
- Host Linux (WSL2) + C nativo / wasm3 / WAMR (interprete + AOT)
- Host Android + C nativo / wasm3 / WAMR (interprete + AOT, in preparazione)

Il repository fornisce:
- codice sorgente completo per tutti gli stack testati
- codice FFT C portabile (radix-2 Cooley–Tukey)
- bytecode WASM e AOT (WAMR)
- istruzioni e comandi per generare gli header .h a partire dai file WASM/AOT
- metodologia di misura reale
- risultati sperimentali su toggling e FFT

<br>

## Hardware utilizzato

| Scheda | MCU | Frequenza core | Oscilloscope | Sonda | Collegamenti |
|--------|-----|----------------|--------------|-------|--------------|
| **Nucleo-F446RE** | STM32F446RET6 (Cortex-M4F) | **180 MHz** | Rigol MSO5104 | ×10 | punta → PA5, GND → GND |
| **Nucleo-F746ZG** | STM32F746ZGT6 (Cortex-M7) | **216 MHz** | Rigol MSO5104 | ×10 | punta → PA5, GND → GND |

Per i test host:

- Linux (WSL2): Ubuntu su WSL2, CPU Intel Core i5‑9600K fissata a 3,7 GHz (Turbo Boost disabilitato).

- Android: dispositivo ARMv8‑A (dettagli hardware e versione Android saranno documentati insieme ai risultati FFT).

<br>

## Obiettivo del benchmark

1. **Toggle benchmark**  
   Misurare la frequenza massima di scrittura su GPIO (BSRR) in un loop infinito, per valutare l'overhead dei diversi runtime.

2. **FFT benchmark**  
   Misurare le prestazioni di una FFT complessa radix-2 in-place (N=1024):  
   - implementata in C portabile puro
   - identica per i test nativi e per i runtime Wasm  
   - compilata:  
     - ARM nativo tramite GCC (STM32CubeIDE)  
     - Wasm interprete tramite clang → wasm32  
     - WAMR AOT tramite wamrc --target=thumbv7em
     - Host Linux/Android tramite toolchain nativa + clang → wasm32 + WAMR AOT (x86‑64/ARMv8)  

La FFT comprende bit-reversal, 10 stadi, twiddle factors precomputati e tutto il flusso Cooley–Tukey DIT.

<br>


## Utilizzo di memoria (Flash/RAM)

In ambito embedded, Flash e RAM rappresentano vincoli critici. Di seguito viene analizzato il footprint di memoria dei runtime su Zephyr, distinguendo tra occupazione statica (compile-time) e dinamica (run-time).

### Metodologia

- **Flash (ROM)**: dimensione totale dell'immagine firmware (`text + rodata + data`), includendo kernel RTOS, runtime Wasm e applicazione.
- **RAM Statica**: dati globali e buffer allocati staticamente (`data + bss`).
- **RAM Dinamica**: heap e stack richiesti a runtime.
- **RAM Totale**: somma effettiva delle risorse impegnate.

**Nota**: il simbolo `*` indica configurazioni in cui parte della memoria “dinamica” risulta contabilizzata come statica (es. pool statico WAMR o heap di sistema Zephyr conteggiato staticamente).

---

### Toggle – Footprint memoria (F446RE)

| Configurazione | Flash (ROM) | RAM Statica | RAM Dinamica | RAM Totale |
|---|---:|---:|---:|---:|
| Zephyr + Wasm3 | 77,80 KiB | 4,69 KiB | ~12,00 KiB¹ | ~16,69 KiB |
| Zephyr + WAMR (Interp) | 85,16 KiB | 77,81 KiB * | Inclusa * | ~77,81 KiB |
| Zephyr + WAMR (AOT) | 78,52 KiB | 80,31 KiB * | Inclusa * | ~80,31 KiB |

<small>¹ Stima: Heap Wasm (8 KiB) + Stack Thread (4 KiB).</small>  
<small>* Include buffer statico pre-allocato (es. pool) contabilizzato nella RAM statica.</small>

---

### Toggle – Footprint memoria (F746ZG)

| Configurazione | Flash (ROM) | RAM Statica | RAM Dinamica | RAM Totale |
|---|---:|---:|---:|---:|
| Zephyr + Wasm3 | 78,36 KiB | 4,75 KiB | ~12,00 KiB¹ | ~16,75 KiB |
| Zephyr + WAMR (Interp) | 85,64 KiB | 77,87 KiB * | Inclusa * | ~77,87 KiB |
| Zephyr + WAMR (AOT) | 78,98 KiB | 80,37 KiB * | Inclusa * | ~80,37 KiB |

<small>¹ Stima: Heap Wasm (8 KiB) + Stack Thread (4 KiB).</small>  
<small>* Include buffer statico pre-allocato (es. pool) contabilizzato nella RAM statica.</small>

---

### FFT – Footprint memoria (F446RE, N=1024)

In questa configurazione, WAMR utilizza l'allocatore di sistema (`malloc`) invece del buffer statico.

| Configurazione | Flash (ROM) | RAM Statica | RAM Dinamica | RAM Totale |
|---|---:|---:|---:|---:|
| Zephyr + Wasm3 | 98,19 KiB | 37,13 KiB * | Inclusa * | ~37,13 KiB |
| Zephyr + WAMR (Interp) | 106,02 KiB | 19,38 KiB | 16,00 KiB | ~35,38 KiB |
| Zephyr + WAMR (AOT) | 99,50 KiB | 22,13 KiB | 16,00 KiB | ~38,13 KiB |

<small>* Wasm3: l'heap dinamico risulta conteggiato nella RAM statica da Zephyr (es. `CONFIG_HEAP_MEM_POOL_SIZE`).</small>

---

### FFT – Footprint memoria (F746ZG, N=1024)

| Configurazione | Flash (ROM) | RAM Statica | RAM Dinamica | RAM Totale |
|---|---:|---:|---:|---:|
| Zephyr + Wasm3 | 98,68 KiB | 37,25 KiB * | Inclusa * | ~37,25 KiB |
| Zephyr + WAMR (Interp) | 106,81 KiB | 19,37 KiB | 16,00 KiB | ~35,37 KiB |
| Zephyr + WAMR (AOT) | 99,99 KiB | 22,25 KiB | 16,00 KiB | ~38,25 KiB |

<small>* Wasm3: l'heap dinamico risulta conteggiato nella RAM statica da Zephyr (es. `CONFIG_HEAP_MEM_POOL_SIZE`).</small>



<br>

## Codice WebAssembly utilizzato

### WAT minimale (toggle)

```wat
(module
  (import "env" "gpio_toggle" (func $gpio_toggle))
  (func (export "toggle_forever")
    (loop $L
      call $gpio_toggle
      br $L)))
```

### Compilazione in .wasm

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

## FFT: implementazione utilizzata
La FFT utilizzata è:
- radix-2 Cooley–Tukey DIT
- complessa
- buffer in-place interlacciato [re, im, re, im, ...]
- N = 1024
- 10 stadi completi
- twiddle precomputati (nessuna dipendenza da math.h)
- identica per ARM nativo, Wasm3, WAMR (interprete + AOT), host Linux/Android.

Il modulo Wasm espone:
```c
__attribute__((export_name("fft_init")))
void fft_init(void);

__attribute__((export_name("fft_bench")))
void fft_bench(int iterations);
```

Compilazione:
```bash
clang --target=wasm32-unknown-unknown \
  -O3 -nostdlib \
  -Wl,--no-entry \
  -Wl,-z,stack-size=16384 \
  -o fft_bench.wasm \
  fft_bench_wasm.c

xxd -i fft_bench.wasm > fft_bench.wasm.h
```

AOT per MCU (ARM Thumb-2):
```bash
wamrc --target=thumbv7em --target-abi=eabi -o fft_bench.aot fft_bench.wasm

xxd -i fft_bench.aot > fft_bench_aot.h
```

AOT per x86_64 Linux:
```bash
wamrc --target=x86_64 \
  --opt-level=3 --size-level=0 \
  --bounds-checks=0 --stack-bounds-checks=0 \
  -o fft_bench.aot fft_bench.wasm

xxd -i fft_bench.aot > fft_bench_aot.h
```

<br>

## Risultati sperimentali – Toggle Benchmark (F446RE, ~180 MHz core)

| Stack            | Runtime / Modalità | Frequenza misurata | Slowdown vs C bare-metal |
|------------------|--------------------|--------------------|--------------------------|
| Bare-metal       | C nativo           | **~36.4 MHz**      | **1.00×**                |
| FreeRTOS         | C nativo           | **~36.4 MHz**      | **1.00×**                |
| Zephyr           | C nativo           | **~36 MHz**        | **1.01×**                |
| Bare-metal       | wasm3 (interprete) | **~502 kHz**       | **72.5×**                |
| FreeRTOS         | wasm3 (interprete) | **~564 kHz**       | **64.5×**                |
| Zephyr           | wasm3 (interprete) | **~523 kHz**       | **69.6×**                |
| Zephyr           | WAMR (interprete)  | **~207 kHz**       | **175.8×**               |
| Zephyr           | WAMR (AOT)         | **~346 kHz**       | **105.2×**               |


**Note Toggle F446RE:**

- I tre casi “nativi” (Bare-metal, FreeRTOS, Zephyr) sono allineati a ~36 MHz, in pratica al limite di quanto il core riesce a togglare scrivendo su BSRR.
- L’overhead del kernel (FreeRTOS/Zephyr) è trascurabile rispetto al loop tight di toggle.
- Wasm3 introduce un fattore di rallentamento ≈ **70×** rispetto al nativo.
- WAMR interprete è più lento di Wasm3 nel setup corrente.
- WAMR AOT migliora nettamente sul suo interprete, ma resta più lento di Wasm3 con la configurazione attuale.
- Anomalia: wasm3 sembra più veloce su un rtos rispetto al baremetal (da indagare bene)
- WAMR non è disponibile baremetal su nessuna piattaforma, è disponibile per zephyr su nucleo, è disponibile su freertos solo per esp-idf
- AOT significa compilare il codice wasm in codice nativo; viene utilizzato solo il runtime wasm che si occupa di gestire il processo, ma non c'è interpretazione

<br>

## Risultati sperimentali – Toggle Benchmark (F746ZG, ~216 MHz core)


| Stack            | Runtime / Modalità | Frequenza misurata | Slowdown vs C nativo |
|------------------|--------------------|--------------------|----------------------|
| Bare-metal       | C nativo           | **~110 MHz**       | **1.00×**            |
| FreeRTOS         | C nativo           | **~110 MHz**       | **1.00×**            |
| Zephyr           | C nativo           | **~108 MHz**       | **1.02×**            |
| Bare-metal       | wasm3 (interprete) | **~908 kHz**       | **121.1×**           |
| FreeRTOS         | wasm3 (interprete) | **~907 kHz**       | **121.3×**           |
| Zephyr           | wasm3 (interprete) | **~1.07 MHz**      | **102.8×**           |
| Zephyr           | WAMR (interprete)  | **~334 kHz**       | **329.3×**           |
| Zephyr           | WAMR (AOT)         | **~467 kHz**       | **235.6×**           |

**Note Toggle F746ZG:**
- Cortex-M7 nativo ~3× più veloce dell'F4 (110 MHz vs 36 MHz).
- Overhead kernel (FreeRTOS/Zephyr) trascurabile sul loop tight.
- Wasm3 F7 ~1.8× più veloce di F4 (908 kHz vs 502 kHz).
- WAMR AOT resta più lento di Wasm3 (~2.3× slowdown relativo).

<br>

## FFT Benchmark (F446RE, N = 1024, 100 iterazioni)
Metrica: cicli medi per una FFT a 1024 punti, ottenuti contando i cicli totali di 100 esecuzioni consecutive dell’algoritmo e dividendo per il numero di iterazioni.

| Ambiente     | Runtime / Modalità       | Cicli medi per FFT | Slowdown vs C bare-metal |
|--------------|--------------------------|-------------------:|-------------------------:|
| Bare-metal   | C nativo                 | **219 022**        | **1.00×**                |
| FreeRTOS     | C nativo                 | **219 009**        | **1.00×**                |
| Zephyr       | C nativo                 | **228 196**        | **1.04×**                |
| Bare-metal   | wasm3 (interprete)       | **17 566 414**     | **80.20×**               |
| FreeRTOS     | wasm3 (interprete)       | **16 103 862**     | **73.53×**               |
| Zephyr       | wasm3 (interprete)       | **14 975 767**     | **68.38×**               |
| Zephyr       | WAMR (interprete)        | **13 567 746**     | **61.95×**               |
| Zephyr       | WAMR (AOT)               | **3 341 702**      | **15.26×**               |                 

**Note FFT F446RE:**
- Interpreti wasm 60–80× slowdown.
- WAMR AOT riduce a ~15×.
- FreeRTOS/Zephyr overhead nullo.

<br>


## FFT Benchmark (F746ZG, N = 1024, 100 iterazioni)

Metrica: cicli medi per una FFT a 1024 punti, ottenuti contando i cicli totali di 100 esecuzioni consecutive dell’algoritmo e dividendo per il numero di iterazioni.

| Ambiente     | Runtime / Modalità       | Cicli medi per FFT | Slowdown vs C bare-metal |
|--------------|--------------------------|-------------------:|-------------------------:|
| Bare-metal   | C nativo                 | **124 108**        | **1.00×**                |
| FreeRTOS     | C nativo                 | **124 562**        | **1.00×**                |
| Zephyr       | C nativo                 | **171 412**        | **1.38×**                |
| Bare-metal   | wasm3 (interprete)       | **15 932 553**     | **128.4×**               |
| FreeRTOS     | wasm3 (interprete)       | **13 962 748**     | **112.5×**               |
| Zephyr       | wasm3 (interprete)       | **14 270 967**     | **115.0×**               |
| Zephyr       | WAMR (interprete)        | **8 759 838**      | **70.6×**                |
| Zephyr       | WAMR (AOT)               | **2 542 771**      | **20.5×**                |


**Note FFT F746ZG:**
- C nativo Bare-metal/FreeRTOS ~1.8× più veloce dell'F4 (124k vs 219k cicli).
- Zephyr C nativo ha ~38% overhead vs bare-metal.
- Tra gli interpreti Wasm, WAMR è il più veloce su F7 (~8.8M cicli, ~70× slowdown), seguito da wasm3 (~14–16M cicli).
- WAMR AOT riduce ulteriormente il gap, a ~20× slowdown rispetto al C nativo.

<br>

## FFT Benchmark – Linux (WSL2, N = 1024, 100 iterazioni)

Metrica: cicli medi equivalenti per una FFT a 1024 punti, calcolati da misure in
nanosecondi su Ubuntu in WSL2, con CPU Intel Core i5-9600K fissata a 3,7 GHz (Turbo Boost disabilitato).

| Ambiente     | Runtime / Modalità   | Cicli medi per FFT  | Slowdown vs C nativo  |
|--------------|----------------------|--------------------:|----------------------:|
| Linux (WSL2) | C nativo             | 28 875              | 1.0×                  |
| Linux (WSL2) | wasm3 (interprete)   | 784 509             | 27.2×                 |
| Linux (WSL2) | WAMR (interprete)    | 1 448 892           | 50.2×                 |
| Linux (WSL2) | WAMR (AOT)           | 50 577              | 1.75×                 |

**Note FFT Linux (WSL2):**
- C nativo resta la baseline più veloce con ~28.9k cicli per FFT.
- wwasm3 introduce uno slowdown di ~27× rispetto al C nativo, mentre WAMR interprete sale a ~50×.
- WAMR AOT resta molto vicino al nativo (~1.75×), in linea con il fatto che AOT su host tende a fornire prestazioni quasi native.

<br>

## FFT Benchmark – Android (N = 1024, 100 iterazioni)

_I risultati per Android (C nativo, wasm3, WAMR interprete e WAMR AOT) verranno aggiunti dopo l’esecuzione dei benchmark sulla piattaforma target._

<br>

## Toolchain utilizzata

- Bare-metal/FreeRTOS: STM32CubeIDE
- Zephyr: Zephyr SDK + west
- WASM:
  - WABT(wat2wasm)
  - clang (compilazione wasm32)
  - xxd
  - Wasm3
  - WAMR(Interpreter + AOT)
  - wamrc (AOT → ARM Thumb2 / x86‑64 / ARMv8 a seconda del target)

<br>

## Build e flash (Bare-metal e FreeRTOS)

Progetti importabili direttamente in STM32CubeIDE.

<br>

## Build e flash in Zephyr

Installare Zephyr SDK + west (linee guida ufficiali):
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
