#include <stm32f4xx.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "bh_platform.h"
#include "bh_assert.h"
#include "bh_log.h"
#include "wasm_export.h"

#include "fft_bench_aot.h"   // generato da xxd -i

/* Config WAMR */
#define CONFIG_GLOBAL_HEAP_BUF_SIZE WASM_GLOBAL_HEAP_SIZE
#define CONFIG_APP_STACK_SIZE       8192
#define CONFIG_APP_HEAP_SIZE        8192
#define CONFIG_MAIN_THREAD_STACK_SIZE 8192

#define NUM_ITER  100

static int app_argc;
static char **app_argv;

/* Risultati benchmark */
static volatile uint32_t total_cycles = 0;
static volatile uint32_t avg_cycles   = 0;

/* ===== DWT cycle counter ===== */
static void DWT_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; // abilita DWT
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ===== Esegue init_buffer + bench(NUM_ITER) nel modulo Wasm e misura i cicli ===== */
static void
run_fft_bench(wasm_module_inst_t module_inst)
{
    const char *exception;
    wasm_function_inst_t fn_init = NULL;
    wasm_function_inst_t fn_bench = NULL;
    wasm_exec_env_t exec_env = NULL;
    uint32 argv[1];

    /* lookup funzioni esportate */
    fn_init = wasm_runtime_lookup_function(module_inst, "fft_init");
    if (!fn_init) {
        printk("Failed to find init_buffer\n");
        return;
    }

    fn_bench = wasm_runtime_lookup_function(module_inst, "fft_bench");
    if (!fn_bench) {
        printk("Failed to find bench\n");
        return;
    }

    /* crea execution env per il modulo */
    exec_env = wasm_runtime_create_exec_env(module_inst, CONFIG_APP_STACK_SIZE);
    if (!exec_env) {
        printk("Create exec env failed\n");
        return;
    }

    /* inizializza il buffer lato Wasm (non misurato) */
    if (!wasm_runtime_call_wasm(exec_env, fn_init, 0, NULL)) {
        exception = wasm_runtime_get_exception(module_inst);
        printk("Exception in init_buffer: %s\n", exception ? exception : "<none>");
        goto out;
    }

    /* prepara DWT e disabilita SysTick/IRQ per una misura comparabile agli altri test */
    DWT_Init();
    SysTick->CTRL = 0;       /* disabilita SysTick */
    __disable_irq();

    uint32_t start = DWT->CYCCNT;

    /* bench(NUM_ITER) */
    argv[0] = (uint32)NUM_ITER;
    if (!wasm_runtime_call_wasm(exec_env, fn_bench, 1, argv)) {
        __enable_irq();
        exception = wasm_runtime_get_exception(module_inst);
        printk("Exception in bench: %s\n", exception ? exception : "<none>");
        goto out;
    }

    uint32_t end = DWT->CYCCNT;

    __enable_irq();

    total_cycles = end - start;
    avg_cycles   = total_cycles / NUM_ITER;

    printk("Zephyr + WAMR(AOT)\n");
    printk("Total cycles: %lu\n", (unsigned long)total_cycles);
    printk("Avg cycles per FFT: %lu\n", (unsigned long)avg_cycles);

out:
    wasm_runtime_destroy_exec_env(exec_env);
}


/* ===== Thread che inizializza WAMR e lancia il benchmark ===== */
void
iwasm_main(void *arg1, void *arg2, void *arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;

    uint8 *wasm_file_buf;
    uint32 wasm_file_size;
    wasm_module_t wasm_module = NULL;
    wasm_module_inst_t wasm_module_inst = NULL;
    RuntimeInitArgs init_args;
    char error_buf[128];

#if WASM_ENABLE_LOG != 0
    int log_verbose_level = 0; /* 0 per non sporcare la misura */
#endif

memset(&init_args, 0, sizeof(RuntimeInitArgs));

init_args.mem_alloc_type = Alloc_With_System_Allocator;

    /* nessun modulo nativo necessario per fft_bench.wasm */
    init_args.native_module_name  = NULL;
    init_args.native_symbols      = NULL;
    init_args.n_native_symbols    = 0;

    if (!wasm_runtime_full_init(&init_args)) {
        printk("Init runtime failed\n");
        return;
    }

#if WASM_ENABLE_LOG != 0
    bh_log_set_verbose_level(log_verbose_level);
#endif

    /* usa il blob incluso fft_bench.wasm */
    wasm_file_buf  = (uint8 *)fft_bench_aot;
    wasm_file_size = (uint32)fft_bench_aot_len;

    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_size,
                                          error_buf, sizeof(error_buf)))) {
        printk("Load module failed: %s\n", error_buf);
        goto fail1;
    }

    if (!(wasm_module_inst = wasm_runtime_instantiate(
              wasm_module, CONFIG_APP_STACK_SIZE, CONFIG_APP_HEAP_SIZE,
              error_buf, sizeof(error_buf)))) {
        printk("Instantiate failed: %s\n", error_buf);
        goto fail2;
    }

    /* esegue il benchmark FFT-like */
    run_fft_bench(wasm_module_inst);

    wasm_runtime_deinstantiate(wasm_module_inst);

fail2:
    wasm_runtime_unload(wasm_module);

fail1:
    wasm_runtime_destroy();

    /* puoi mettere un loop vuoto per fermare il thread */
    while (1) {
        k_sleep(K_FOREVER);
    }
}

/* ===== Creazione thread WAMR ===== */
#define MAIN_THREAD_STACK_SIZE (CONFIG_MAIN_THREAD_STACK_SIZE)
#define MAIN_THREAD_PRIORITY   5

K_THREAD_STACK_DEFINE(iwasm_main_thread_stack, MAIN_THREAD_STACK_SIZE);
static struct k_thread iwasm_main_thread;

bool
iwasm_init(void)
{
    k_tid_t tid = k_thread_create(
        &iwasm_main_thread,
        iwasm_main_thread_stack,
        MAIN_THREAD_STACK_SIZE,
        iwasm_main,
        NULL, NULL, NULL,
        MAIN_THREAD_PRIORITY,
        0,
        K_NO_WAIT);

    return tid ? true : false;
}

/* ===== Entry Zephyr ===== */
void
main(void)
{
    /* qui niente GPIO toggle, solo benchmark WAMR */
    (void)iwasm_init();
    /* main può dormire, il lavoro lo fa il thread iwasm_main */
    while (1) {
        k_sleep(K_FOREVER);
    }
}
