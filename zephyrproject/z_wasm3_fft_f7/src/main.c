#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <stm32f7xx.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys_clock.h>

#if __has_include(<wasm3.h>)
#include <wasm3.h>
#elif __has_include(<m3/wasm3.h>)
#include <m3/wasm3.h>
#else
#include "wasm3.h"
#endif

#if __has_include(<m3_api_defs.h>)
#include <m3_api_defs.h>
#elif __has_include(<m3/m3_api_defs.h>)
#include <m3/m3_api_defs.h>
#else
#include "m3_api_defs.h"
#endif

#include "fft_bench.wasm.h"

#define NUM_ITER  100

volatile uint32_t total_cycles_wasm = 0;
volatile uint32_t avg_cycles_wasm   = 0;


void enable_caches(void) {
    SCB_EnableICache();
    SCB_EnableDCache();
}

void enable_prefetch(void) {
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
}

static void wasm_panic(const char* where, M3Result r) {
    // Usa printf se hai il retarget via UART,
    // altrimenti metti un breakpoint qui e guarda 'where' e 'r' in debug
    printk("Wasm error in %s: %s\r\n", where, r);
    //__disable_irq();
    while (1) {}
}

static void run_wasm_fft_benchmark(void) {
	M3Result r;

	IM3Environment env = m3_NewEnvironment();
	if (!env) wasm_panic("m3_NewEnvironment", "OOM");

	IM3Runtime rt = m3_NewRuntime(env, 1024, NULL);
	if (!rt) wasm_panic("m3_NewRuntime", "OOM");
     
	IM3Module mod = NULL;
	r = m3_ParseModule(env, &mod,
			(const uint8_t*)fft_bench_wasm,
			fft_bench_wasm_len);
	if (r) wasm_panic("m3_ParseModule", r);
       
	r = m3_LoadModule(rt, mod);
	if (r) wasm_panic("m3_LoadModule", r);
    
	// Il modulo FFT non ha import host, quindi niente m3_LinkRawFunction

	IM3Function fn_init = NULL;
	IM3Function fn_bench = NULL;
	

	r = m3_FindFunction(&fn_init, rt, "fft_init");
	if (r || !fn_init) wasm_panic("m3_FindFunction(init_buffer)", r);

	r = m3_FindFunction(&fn_bench, rt, "fft_bench");
	if (r || !fn_bench) wasm_panic("m3_FindFunction(bench)", r);



	// Inizializzazione DWT e disabilitazione SysTick/IRQ per confronto pulito
	
	//SysTick->CTRL = 0;    // disabilita SysTick (come nei test nativi)

	// Inizializza il buffer dentro il modulo Wasm
	r = m3_CallV(fn_init);
	if (r) wasm_panic("m3_CallV(init_buffer)", r);

	//__disable_irq();

	uint32_t start = k_cycle_get_32();

	// bench(NUM_ITER) dentro Wasm
	r = m3_CallV(fn_bench, (uint32_t)NUM_ITER);
	if (r) {
		//__enable_irq();
		wasm_panic("m3_CallV(bench)", r);
	}

	uint32_t end = k_cycle_get_32();

	//__enable_irq();

	total_cycles_wasm = end - start;
	avg_cycles_wasm   = total_cycles_wasm / NUM_ITER;
	printk("Zephyr + wasm3\r\n");
	printk("Total cycles: %lu\r\n", (unsigned long)total_cycles_wasm);
	printk("Avg cycles per FFT: %lu\r\n", (unsigned long)avg_cycles_wasm);

	
	// cleanup opzionale (tanto poi non usciamo da main)
	m3_FreeRuntime(rt);
	m3_FreeEnvironment(env);
}

void main(void) {
    enable_caches();
    enable_prefetch();
    run_wasm_fft_benchmark(); 
    
    while (1) {
        __NOP();
    }
}
