#include "wasm3.h"
#include <zephyr/kernel.h>
#include "mod_b_wasm.h"
#include "mod_a_wasm.h"

K_THREAD_STACK_DEFINE(stack_a, 1024*14);
K_THREAD_STACK_DEFINE(stack_b, 1024*14);
static struct k_thread thread_a, thread_b;

// Definisce un semaforo chiamato 'uart_sem' con valore iniziale 1 e massimo 1
K_SEM_DEFINE(uart_sem, 1, 1);

static const void* uart_print_wasm(IM3Runtime rt, IM3ImportContext ctx, uint64_t *sp, void *mem) {
    uint32_t offset = (uint32_t)sp[0];
    uint8_t * base_mem = m3_GetMemory(rt, 0, 0);
    
    if (base_mem) {
        char buffer[32] = "";
        uint32_t idx = 0;
        for (uint32_t i = 0; i < 30; i++) { // Aumentato safe limit a 30
            uint8_t byte = base_mem[offset + i];
            buffer[idx++] = (char)byte;
            if (byte == 0) break;
        }
        buffer[idx] = '\0';

        // --- INIZIO SEZIONE CRITICA ---
        // Attende all'infinito finché il semaforo non è libero
        k_sem_take(&uart_sem, K_FOREVER);
        
        printk("%s", buffer);
        
        // Rilascia il semaforo per l'altro thread
        k_sem_give(&uart_sem);
        // --- FINE SEZIONE CRITICA ---
    }
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
    M3Result result = m3_ParseModule(env, &mod, mod_a_wasm, mod_a_wasm_len);

    if (result) {
        printk("PARSE FAIL: %s\n", result); // Stampa l'errore (probabilmente "malloc failed")
        return;
    }

    result = m3_LoadModule(rt, mod);
    if (result) {
        printk("LOAD FAIL: %s\n", result);
        return;
    }

    m3_LinkRawFunction(mod, "env", "uart_print", "v(*)", uart_print_wasm);
    IM3Function step;
    m3_FindFunction(&step, rt, "step");
    
    printk("WASM READY\n");
    while (1) {
        m3_CallV(step);
        //k_sleep(K_MSEC(1000));
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
    M3Result result = m3_ParseModule(env, &mod, mod_b_wasm, mod_b_wasm_len);

    if (result) {
        printk("PARSE FAIL: %s\n", result); // Stampa l'errore (probabilmente "malloc failed")
        return;
    }

    result = m3_LoadModule(rt, mod);
    if (result) {
        printk("LOAD FAIL: %s\n", result);
        return;
    }

    m3_LinkRawFunction(mod, "env", "uart_print", "v(*)", uart_print_wasm);
    IM3Function step;
    m3_FindFunction(&step, rt, "step");
    
    printk("WASM READY\n");
    while (1) {
        m3_CallV(step);
        //k_sleep(K_MSEC(1000));
    }
    // IDENTICO ma mod_b_wasm + k_sleep(K_MSEC(1000));
}

int main(void) {
    printk("Zephyr DUAL WASM THREADS\n");
    
    // Rimosso "k_tid_t tid_a =" perché inutilizzato
    k_thread_create(&thread_a, stack_a, K_THREAD_STACK_SIZEOF(stack_a),
                    run_wasm_a, NULL, NULL, NULL, 4, 0, K_NO_WAIT);

    // Rimosso "k_tid_t tid_b =" perché inutilizzato
    k_thread_create(&thread_b, stack_b, K_THREAD_STACK_SIZEOF(stack_b),
                    run_wasm_b, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    
    printk("DUAL THREADS CREATE OK\n");
    k_sleep(K_FOREVER);
    return 0; // Aggiunto return
}
