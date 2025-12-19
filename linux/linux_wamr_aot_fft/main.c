#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include "wasm_export.h"
#include "fft_bench_aot.h"

static uint64_t ns_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(void)
{
    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    wasm_module_inst_t inst = NULL;
    wasm_exec_env_t env = NULL;
    char error_buf[128];
    const char *exception;
    uint64_t cpu_hz = 3700000000ULL; // 3.7 GHz
    uint64_t start, end, total_ns, total_cycles, avg_cycles;
    const uint32_t NUM_ITER = 100;
    uint8_t *wasm_file_buf;
    uint32_t wasm_file_size;

    /* init args base: usa allocatore di sistema */
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;

    if (!wasm_runtime_full_init(&init_args)) {
        printf("wasm_runtime_full_init failed\n");
        return -1;
    }


    wasm_file_buf  = (uint8_t *)fft_bench_aot;
    wasm_file_size = (uint32_t)fft_bench_aot_len;

    /* carica modulo da array embed‑dato */
    module = wasm_runtime_load(wasm_file_buf, wasm_file_size, error_buf, sizeof(error_buf));
    if (!module) {
        printf("load failed: %s\n", error_buf);
        goto quit;
    }

    /* istanzia modulo */
    inst = wasm_runtime_instantiate(module,
                                    64 * 1024,   /* stack */
                                    16 * 1024,   /* heap */
                                    error_buf, sizeof(error_buf));
    if (!inst) {
        printf("instantiate failed: %s\n", error_buf);
        goto quit;
    }

    env = wasm_runtime_get_exec_env_singleton(inst);

    wasm_function_inst_t f_init =
        wasm_runtime_lookup_function(inst, "fft_init");
    wasm_function_inst_t f_bench =
        wasm_runtime_lookup_function(inst, "fft_bench");

    if (!f_init || !f_bench) {
        printf("functions not found\n");
        goto quit;
    }

    /* fft_init() */
    if (!wasm_runtime_call_wasm(env, f_init, 0, NULL)) {
        exception = wasm_runtime_get_exception(inst);
        printf("exception in fft_init: %s\n", exception);
        goto quit;
    }

    /* fft_bench(NUM_ITER) */
    uint32_t argv_wasm[1] = { NUM_ITER };

    start = ns_now();
    if (!wasm_runtime_call_wasm(env, f_bench, 1, argv_wasm)) {
        exception = wasm_runtime_get_exception(inst);
        printf("exception in fft_bench: %s\n", exception);
        goto quit;
    }
    end = ns_now();

    total_ns = end - start;
    total_cycles = total_ns * (cpu_hz / 1000000000ULL);
    avg_cycles   = total_cycles / NUM_ITER;

    printf("Linux + WAMR (interp)\r\n");
	printf("Total cycles: %" PRIu64 "\n", total_cycles);
    printf("Avg cycles per FFT: %" PRIu64 "\n", avg_cycles);

quit:
    if (inst)
        wasm_runtime_deinstantiate(inst);
    if (module)
        wasm_runtime_unload(module);
    wasm_runtime_destroy();
    return 0;
}
