#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>

#include "wasm_export.h"
#include "toggle_a_wasm.h"
#include "toggle_b_wasm.h"

/* 2 thread host */
K_THREAD_STACK_DEFINE(stack_a, 1024 * 14);
K_THREAD_STACK_DEFINE(stack_b, 1024 * 14);
static struct k_thread thread_a, thread_b;

/* Semaforo per serializzare accesso LED */
K_SEM_DEFINE(led_sem, 1, 1);

/* LED0 da device tree */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Native import: env.led_toggle(i32 duration_ms) */
static void led_toggle_native(wasm_exec_env_t exec_env, uint32_t duration_ms)
{
    ARG_UNUSED(exec_env);

    k_sem_take(&led_sem, K_FOREVER);

    printk("LED ON (thread %p)\n", k_current_get());
    gpio_pin_set_dt(&led, 1);
    k_sleep(K_MSEC(duration_ms));
    printk("LED OFF (thread %p)\n", k_current_get());
    gpio_pin_set_dt(&led, 0);

    k_sem_give(&led_sem);
}

/* "(i)" = 1 argomento i32, return void */
static NativeSymbol native_symbols[] = {
    { "led_toggle", (void *)led_toggle_native, "(i)" },
};

struct wamr_task_args {
    const char *name;
    int prio;
    const uint8_t *wasm_buf;
    uint32_t wasm_len;
};

static void run_wamr_toggle(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct wamr_task_args *args = (struct wamr_task_args *)p1;
    char error_buf[128];

    printk("=== %s START ===\n", args->name);

    wasm_runtime_init_thread_env();

    wasm_module_t module =
        wasm_runtime_load(args->wasm_buf, args->wasm_len,
                          error_buf, sizeof(error_buf));
    if (!module) {
        printk("[%s] load FAIL: %s\n", args->name, error_buf);
        goto out;
    }

    const uint32_t wasm_stack_size = 4 * 1024;
    const uint32_t host_managed_heap_size = 0; /* toggle non usa heap */

    wasm_module_inst_t inst =
        wasm_runtime_instantiate(module, wasm_stack_size, host_managed_heap_size,
                                 error_buf, sizeof(error_buf));
    if (!inst) {
        printk("[%s] instantiate FAIL: %s\n", args->name, error_buf);
        wasm_runtime_unload(module);
        goto out;
    }

    wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst, 2 * 1024);
    if (!exec_env) {
        printk("[%s] create_exec_env FAIL\n", args->name);
        wasm_runtime_deinstantiate(inst);
        wasm_runtime_unload(module);
        goto out;
    }

    wasm_function_inst_t step = wasm_runtime_lookup_function(inst, "step");
    if (!step) {
        printk("[%s] lookup step FAIL\n", args->name);
        wasm_runtime_destroy_exec_env(exec_env);
        wasm_runtime_deinstantiate(inst);
        wasm_runtime_unload(module);
        goto out;
    }

    printk("[%s] WAMR READY\n", args->name);

    while (1) {
        if (!wasm_runtime_call_wasm(exec_env, step, 0, NULL)) {
            const char *ex = wasm_runtime_get_exception(inst);
            printk("[%s] wasm exception: %s\n", args->name, ex ? ex : "(null)");
            k_sleep(K_MSEC(500));
        }
        k_sleep(K_MSEC(1000));
    }

out:
    wasm_runtime_destroy_thread_env();
    k_sleep(K_FOREVER);
}

int main(void)
{
    printk("Zephyr DUAL WAMR GPIO TOGGLE (A/B)\n");

    if (!gpio_is_ready_dt(&led)) {
        printk("Error: LED device not ready\n");
        return 0;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    if (!wasm_runtime_init()) {
        printk("wasm_runtime_init FAIL\n");
        return 0;
    }

    int n_native = (int)(sizeof(native_symbols) / sizeof(native_symbols[0]));
    if (!wasm_runtime_register_natives("env", native_symbols, n_native)) {
        printk("wasm_runtime_register_natives FAIL\n");
        return 0;
    }
    /* register_natives prima del load() */
    /* (requirement WAMR) */
    /* [web:60] */

    static struct wamr_task_args a, b;

    a.name = "TASK A";
    a.prio = 5;
    a.wasm_buf = toggle_a_wasm;
    a.wasm_len = (uint32_t)toggle_a_wasm_len;

    b.name = "TASK B";
    b.prio = 5;
    b.wasm_buf = toggle_b_wasm;
    b.wasm_len = (uint32_t)toggle_b_wasm_len;

    k_thread_create(&thread_a, stack_a, K_THREAD_STACK_SIZEOF(stack_a),
                    run_wamr_toggle, &a, NULL, NULL,
                    a.prio, 0, K_NO_WAIT);

    k_thread_create(&thread_b, stack_b, K_THREAD_STACK_SIZEOF(stack_b),
                    run_wamr_toggle, &b, NULL, NULL,
                    b.prio, 0, K_NO_WAIT);

    printk("DUAL THREADS CREATE OK\n");
    k_sleep(K_FOREVER);
    return 0;
}
