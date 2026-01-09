#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "wasm_export.h"

#include "mod_a_wasm.h"
#include "mod_b_wasm.h"

K_THREAD_STACK_DEFINE(stack_a, 1024 * 14);
K_THREAD_STACK_DEFINE(stack_b, 1024 * 14);
static struct k_thread thread_a, thread_b;

K_SEM_DEFINE(uart_sem, 1, 1);

/* env.uart_print(i32 offset)  */
static void
uart_print_native(wasm_exec_env_t exec_env, uint32_t offset)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env); /* ok dentro native */ /* [web:51] */

    /* Verifica che offset punti a una stringa NUL-terminated valida nella memoria Wasm */
    if (!wasm_runtime_validate_app_str_addr(module_inst, offset)) {
        /* Se fallisce, WAMR setta anche una exception; qui si può solo return */
        return;
    }

    const char *s = (const char *)wasm_runtime_addr_app_to_native(module_inst, offset);
    if (!s) {
        return;
    }

    k_sem_take(&uart_sem, K_FOREVER);
    printk("%s", s);
    k_sleep(K_MSEC(1000));
    k_sem_give(&uart_sem);
}

/* Registrazione native: nota signature "(i)" = 1 argomento i32, return void */
static NativeSymbol native_symbols[] = {
    { "uart_print", (void *)uart_print_native, "(i)" },
};

struct wamr_task_args {
    const uint8_t *wasm_buf;
    uint32_t wasm_len;
    const char *name;
    int prio;
};

static void run_wamr_module(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct wamr_task_args *args = (struct wamr_task_args *)p1;
    char error_buf[128];

    printk("=== %s START ===\n", args->name);

    /* Thread creato dall'host (Zephyr): init thread env per WAMR */
    wasm_runtime_init_thread_env(); /* consigliato in embedding multithread */ /* [web:33] */

    wasm_module_t module =
        wasm_runtime_load(args->wasm_buf, args->wasm_len, error_buf, sizeof(error_buf));
    if (!module) {
        printk("[%s] load FAIL: %s\n", args->name, error_buf);
        goto out;
    }

    const uint32_t wasm_stack_size = 4 * 1024;  /* stack per esecuzione Wasm */
    const uint32_t wamr_heap_size  = 8 * 1024;  /* heap extra per istanza (puoi anche metterlo 0) */

    wasm_module_inst_t module_inst =
        wasm_runtime_instantiate(module, wasm_stack_size, wamr_heap_size,
                                 error_buf, sizeof(error_buf));
    if (!module_inst) {
        printk("[%s] instantiate FAIL: %s\n", args->name, error_buf);
        wasm_runtime_unload(module);
        goto out;
    }

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(module_inst, 2 * 1024);
    if (!exec_env) {
        printk("[%s] create_exec_env FAIL\n", args->name);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        goto out;
    }

    wasm_function_inst_t step =
        wasm_runtime_lookup_function(module_inst, "step");
    if (!step) {
        printk("[%s] lookup step FAIL\n", args->name);
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        goto out;
    }

    printk("[%s] WAMR READY\n", args->name);

    while (1) {
        if (!wasm_runtime_call_wasm(exec_env, step, 0, NULL)) {
            const char *ex = wasm_runtime_get_exception(module_inst);
            printk("[%s] wasm exception: %s\n", args->name, ex ? ex : "(null)");
            k_sleep(K_MSEC(500));
        }

        /* evita di monopolizzare la CPU */
        k_yield();
    }

    /* not reached */
    // wasm_runtime_destroy_exec_env(exec_env);
    // wasm_runtime_deinstantiate(module_inst);
    // wasm_runtime_unload(module);

out:
    wasm_runtime_destroy_thread_env();
    k_sleep(K_FOREVER);
}

int main(void)
{
    printk("Zephyr DUAL WAMR THREADS (uart_print(i32))\n");

    if (!wasm_runtime_init()) {
        printk("wasm_runtime_init FAIL\n");
        return 0;
    }

    int n_native = (int)(sizeof(native_symbols) / sizeof(native_symbols[0]));
    if (!wasm_runtime_register_natives("env", native_symbols, n_native)) {
        printk("wasm_runtime_register_natives FAIL\n");
        return 0;
    }
    /* importante: register_natives prima di load() */
    /* [web:60] */

    static struct wamr_task_args a;
    static struct wamr_task_args b;

    a.wasm_buf = mod_a_wasm;
    a.wasm_len = (uint32_t)mod_a_wasm_len;
    a.name = "TASK A";
    a.prio = 5;

    b.wasm_buf = mod_b_wasm;
    b.wasm_len = (uint32_t)mod_b_wasm_len;
    b.name = "TASK B";
    b.prio = 5;


    k_thread_create(&thread_a, stack_a, K_THREAD_STACK_SIZEOF(stack_a),
                    run_wamr_module, &a, NULL, NULL,
                    a.prio, 0, K_NO_WAIT);

    k_thread_create(&thread_b, stack_b, K_THREAD_STACK_SIZEOF(stack_b),
                    run_wamr_module, &b, NULL, NULL,
                    b.prio, 0, K_NO_WAIT);

    printk("DUAL THREADS CREATE OK\n");
    k_sleep(K_FOREVER);
    return 0;
}
