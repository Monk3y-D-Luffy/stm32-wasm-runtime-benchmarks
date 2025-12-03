#include <stm32f7xx.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "bh_platform.h"
#include "bh_assert.h"
#include "bh_log.h"
#include "wasm_export.h"

#include "toggle_aot.h"

#include <arm_cmse.h> // o core_cm7.h
#include <core_cm7.h>
#include "stm32f7xx_hal.h"  // o h7xx

void enable_caches(void) {
    SCB_EnableICache();
    SCB_EnableDCache();
}

void enable_prefetch(void) {
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
}

#define CONFIG_GLOBAL_HEAP_BUF_SIZE WASM_GLOBAL_HEAP_SIZE
#define CONFIG_APP_STACK_SIZE       8192
#define CONFIG_APP_HEAP_SIZE        8192
#define CONFIG_MAIN_THREAD_STACK_SIZE 8192

static int app_argc;
static char **app_argv;

/* ===== GPIO PA5 init (stesso per tutti gli stack) ===== */
static inline void gpio_pa5_init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIOA->MODER   = (GPIOA->MODER & ~(3u << (5*2))) | (1u << (5*2)); // output
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3u << (5*2))) | (3u << (5*2)); // very high
}

/* ===== Native API: env.gpio_toggle() ===== */
static void
gpio_toggle(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    static uint32_t s;
    s ^= 1u;
    if (s) {
        GPIOA->BSRR = (1u << 5);
    } else {
        GPIOA->BSRR = (1u << (5 + 16));
    }
}

/* Tabella simboli nativi (nome modulo "env") */
static NativeSymbol native_symbols[] = {
    EXPORT_WASM_API_WITH_SIG(gpio_toggle, "()"),
};

/* ===== Esegue la funzione export "toggle_forever" dal modulo Wasm ===== */
static void *
app_instance_main(wasm_module_inst_t module_inst)
{
    const char *exception;
    wasm_function_inst_t func;
    wasm_exec_env_t exec_env;

    /* Cerca toggle_forever nel modulo */
    func = wasm_runtime_lookup_function(module_inst, "toggle_forever");
    if (!func) {
        os_printf("Failed to find toggle_forever\n");
        return NULL;
    }

    /* Crea execution env per il modulo */
    exec_env = wasm_runtime_create_exec_env(module_inst, CONFIG_APP_HEAP_SIZE);
    if (!exec_env) {
        os_printf("Create exec env failed\n");
        return NULL;
    }

    /* Chiamata: non ritorna finché il modulo gira */
    wasm_runtime_call_wasm(exec_env, func, 0, NULL);

    if ((exception = wasm_runtime_get_exception(module_inst)) != NULL) {
        os_printf("Exception: %s\n", exception);
    }

    wasm_runtime_destroy_exec_env(exec_env);
    return NULL;
}

#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
static char global_heap_buf[CONFIG_GLOBAL_HEAP_BUF_SIZE] = { 0 };
#endif

/* ===== Thread che inizializza WAMR e lancia il modulo ===== */
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

#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);
#else
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
#endif

    init_args.native_module_name  = "env";  /* deve matchare l'import Wasm */
    init_args.native_symbols      = native_symbols;
    init_args.n_native_symbols    = sizeof(native_symbols) / sizeof(NativeSymbol);

    if (!wasm_runtime_full_init(&init_args)) {
        os_printf("Init runtime failed\n");
        return;
    }

#if WASM_ENABLE_LOG != 0
    bh_log_set_verbose_level(log_verbose_level);
#endif

    /* usa il blob incluso */
    wasm_file_buf  = (uint8 *)toggle_aot;
    wasm_file_size = (uint32)toggle_aot_len;

    if (!(wasm_module = wasm_runtime_load(wasm_file_buf, wasm_file_size,
                                          error_buf, sizeof(error_buf)))) {
        os_printf("Load module failed: %s\n", error_buf);
        goto fail1;
    }

    if (!(wasm_module_inst = wasm_runtime_instantiate(
              wasm_module, CONFIG_APP_STACK_SIZE, CONFIG_APP_HEAP_SIZE,
              error_buf, sizeof(error_buf)))) {
        os_printf("Instantiate failed: %s\n", error_buf);
        goto fail2;
    }

    /* Esegue toggle_forever → loop di toggle continuo su PA5 */
    app_instance_main(wasm_module_inst);

    wasm_runtime_deinstantiate(wasm_module_inst);

fail2:
    wasm_runtime_unload(wasm_module);

fail1:
    wasm_runtime_destroy();
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
    enable_caches();
    enable_prefetch();
    gpio_pa5_init();
    (void)iwasm_init();
    /* non fare altro: il toggle avviene nel thread WAMR */
}
