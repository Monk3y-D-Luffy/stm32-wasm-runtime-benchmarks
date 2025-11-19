#include <zephyr/kernel.h>
#include <stm32f4xx.h>

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
volatile float    checksum_wasm     = 0.0f;

    
   

static void DWT_Init(void) {
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; // abilita DWT
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void wasm_panic(const char* where, M3Result r) {
    // Usa printf se hai il retarget via UART,
    // altrimenti metti un breakpoint qui e guarda 'where' e 'r' in debug
    printk("Wasm error in %s: %s\r\n", where, r);
    __disable_irq();
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
	IM3Function fn_checksum = NULL;

	r = m3_FindFunction(&fn_init, rt, "init_buffer");
	if (r || !fn_init) wasm_panic("m3_FindFunction(init_buffer)", r);

	r = m3_FindFunction(&fn_bench, rt, "bench");
	if (r || !fn_bench) wasm_panic("m3_FindFunction(bench)", r);

	r = m3_FindFunction(&fn_checksum, rt, "get_checksum");
	if (r || !fn_checksum) wasm_panic("m3_FindFunction(get_checksum)", r);

	// Inizializzazione DWT e disabilitazione SysTick/IRQ per confronto pulito
	DWT_Init();
	SysTick->CTRL = 0;    // disabilita SysTick (come nei test nativi)

	// Inizializza il buffer dentro il modulo Wasm
	r = m3_CallV(fn_init);
	if (r) wasm_panic("m3_CallV(init_buffer)", r);

	__disable_irq();

	uint32_t start = DWT->CYCCNT;

	// bench(NUM_ITER) dentro Wasm
	r = m3_CallV(fn_bench, (uint32_t)NUM_ITER);
	if (r) {
		__enable_irq();
		wasm_panic("m3_CallV(bench)", r);
	}

	uint32_t end = DWT->CYCCNT;

	__enable_irq();

	total_cycles_wasm = end - start;
	avg_cycles_wasm   = total_cycles_wasm / NUM_ITER;
	printk("Zephyr + wasm3\r\n");
	printk("Total cycles: %lu\r\n", (unsigned long)total_cycles_wasm);
	printk("Avg cycles per FFT: %lu\r\n", (unsigned long)avg_cycles_wasm);

	// Leggi anche il checksum per evitare ottimizzazioni e avere un controllo
	r = m3_CallV(fn_checksum);
	if (r) wasm_panic("m3_CallV(get_checksum)", r);

	float checksum = 0.0f;
	r = m3_GetResultsV(fn_checksum, &checksum);
	if (r) wasm_panic("m3_GetResultsV(get_checksum)", r);

	checksum_wasm = checksum;

	// Se vuoi, puoi stampare via UART (se hai _write/printf configurati):
	// printf("WASM FFT-like\r\n");
	// printf("Total cycles: %lu\r\n", (unsigned long)total_cycles_wasm);
	// printf("Avg cycles per FFT: %lu\r\n", (unsigned long)avg_cycles_wasm);
	// printf("Checksum: %f\r\n", checksum_wasm);

	// cleanup opzionale (tanto poi non usciamo da main)
	m3_FreeRuntime(rt);
	m3_FreeEnvironment(env);
}

void main(void) {
    
    run_wasm_fft_benchmark(); 
    
    while (1) {
        __NOP();
    }
}
