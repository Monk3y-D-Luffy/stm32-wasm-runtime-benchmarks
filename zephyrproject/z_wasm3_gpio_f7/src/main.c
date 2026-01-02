#include "wasm3.h"
#include <zephyr/kernel.h>
#include "toggle_wasm.h"
#include <zephyr/drivers/gpio.h>

K_THREAD_STACK_DEFINE(stack_a, 1024*14);
K_THREAD_STACK_DEFINE(stack_b, 1024*14);
static struct k_thread thread_a, thread_b;

// Definisce un semaforo chiamato 'led_sem' con valore iniziale 1 e massimo 1
K_SEM_DEFINE(led_sem, 1, 1);

// Riferimento al LED0 (Green) dal Device Tree standard della Nucleo
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);


// Funzione chiamata da WASM: void led_toggle(int duration_ms)
static const void* m3_led_toggle(IM3Runtime rt, IM3ImportContext ctx, uint64_t *sp, void *mem) {
    uint32_t duration = (uint32_t)sp[0];

    // --- SEZIONE CRITICA ---
    k_sem_take(&led_sem, K_FOREVER); // Usiamo lo stesso semaforo o uno nuovo dedicato

    printk("LED ON (Thread %p)\n", k_current_get());
    gpio_pin_set_dt(&led, 1);
    
    // Mantiene il LED acceso per il tempo richiesto (simula lavoro hardware)
    k_sleep(K_MSEC(duration));
    
    printk("LED OFF (Thread %p)\n", k_current_get());
    gpio_pin_set_dt(&led, 0);

    k_sem_give(&led_sem);
    // --- FINE SEZIONE CRITICA ---

    return NULL;
}



// Task A (mod_a_wasm - Salvatore)
void run_wasm_a(void *a, void *b, void *c) {
    printk("=== TASK A START ===\n");
    printk("=== WASM THREAD START ===\n");
    // Test heap semplice
    void *test = k_malloc(1024);
    if (test) {
        printk("HEAP OK\n");
        k_free(test);
    } else {
        printk("HEAP FAIL\n");
        return;
    }
    
    printk("m3_NewEnvironment...\n");
    IM3Environment env = m3_NewEnvironment();
    if (!env) {
        printk("ENV FAIL\n");
        return;
    }
    printk("ENV OK\n");
    
    printk("m3_NewRuntime...\n");
    IM3Runtime rt = m3_NewRuntime(env, 1024*4, NULL);
    if (!rt) {
        printk("RT FAIL\n");
        return;
    }
    printk("RT OK\n");
    
    printk("ParseModule...\n");
    IM3Module mod;
    M3Result result = m3_ParseModule(env, &mod, toggle_wasm, toggle_wasm_len);

    if (result) {
        printk("PARSE FAIL: %s\n", result); // Stampa l'errore (probabilmente "malloc failed")
        return;
    }

    result = m3_LoadModule(rt, mod);
    if (result) {
        printk("LOAD FAIL: %s\n", result);
        return;
    }

    m3_LinkRawFunction(mod, "env", "led_toggle", "v(i)", m3_led_toggle);
    IM3Function step;
    m3_FindFunction(&step, rt, "step");
    
    printk("WASM READY\n");
    while (1) {
        m3_CallV(step);
        k_sleep(K_MSEC(1000));
    }
    // IDENTICO a run_wasm_single ma mod_a_wasm
    // ... (copia da run_wasm_single)
    // Cambia solo: m3_ParseModule(env, &mod, mod_a_wasm, mod_a_wasm_len);
    // Delay: k_sleep(K_MSEC(1500));
}

// Task B (mod_b_wasm - Messaggio)  
void run_wasm_b(void *a, void *b, void *c) {
    printk("=== TASK B START ===\n");
    printk("=== WASM THREAD START ===\n");
    
    // Test heap semplice
    void *test = k_malloc(1024);
    if (test) {
        printk("HEAP OK\n");
        k_free(test);
    } else {
        printk("HEAP FAIL\n");
        return;
    }
    
    printk("m3_NewEnvironment...\n");
    IM3Environment env = m3_NewEnvironment();
    if (!env) {
        printk("ENV FAIL\n");
        return;
    }
    printk("ENV OK\n");
    
    printk("m3_NewRuntime...\n");
    IM3Runtime rt = m3_NewRuntime(env, 1024*4, NULL);
    if (!rt) {
        printk("RT FAIL\n");
        return;
    }
    printk("RT OK\n");
    
    printk("ParseModule...\n");
    IM3Module mod;
    M3Result result = m3_ParseModule(env, &mod, toggle_wasm, toggle_wasm_len);

    if (result) {
        printk("PARSE FAIL: %s\n", result); // Stampa l'errore (probabilmente "malloc failed")
        return;
    }

    result = m3_LoadModule(rt, mod);
    if (result) {
        printk("LOAD FAIL: %s\n", result);
        return;
    }

    m3_LinkRawFunction(mod, "env", "led_toggle", "v(i)", m3_led_toggle);
    IM3Function step;
    m3_FindFunction(&step, rt, "step");
    
    printk("WASM READY\n");
    while (1) {
        m3_CallV(step);
        k_sleep(K_MSEC(1000));
    }
    // IDENTICO ma mod_b_wasm + k_sleep(K_MSEC(1000));
}

int main(void) {
    if (!gpio_is_ready_dt(&led)) {
    printk("Error: LED device not ready\n");
    return 0;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

    printk("Zephyr DUAL WASM THREADS\n");
    
    // Rimosso "k_tid_t tid_a =" perché inutilizzato
    k_thread_create(&thread_a, stack_a, K_THREAD_STACK_SIZEOF(stack_a),
                    run_wasm_a, NULL, NULL, NULL, 5, 0, K_NO_WAIT);

    // Rimosso "k_tid_t tid_b =" perché inutilizzato
    k_thread_create(&thread_b, stack_b, K_THREAD_STACK_SIZEOF(stack_b),
                    run_wasm_b, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    
    printk("DUAL THREADS CREATE OK\n");
    k_sleep(K_FOREVER);
    return 0; // Aggiunto return
}
