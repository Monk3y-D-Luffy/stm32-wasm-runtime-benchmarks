#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

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

uint64_t cpu_hz = 3700000000ULL; // 3.7 GHz
uint64_t total_ns = 0;
uint64_t total_cycles = 0;
uint64_t avg_cycles = 0;


static void wasm_panic(const char* where, M3Result r) {
    // Usa printf se hai il retarget via UART,
    // altrimenti metti un breakpoint qui e guarda 'where' e 'r' in debug
    printf("Wasm error in %s: %s\r\n", where, r);
    //__disable_irq();
    while (1) {}
}

static uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
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

	

	uint64_t start = ns_now();

	// bench(NUM_ITER) dentro Wasm
	r = m3_CallV(fn_bench, (uint32_t)NUM_ITER);
	if (r) {
		//__enable_irq();
		wasm_panic("m3_CallV(bench)", r);
	}

	uint64_t end   = ns_now();


	total_ns = end - start;
	total_cycles = total_ns * (cpu_hz / 1000000000ULL);
	avg_cycles   = total_cycles / NUM_ITER;
	printf("Linux + wasm3\r\n");
	printf("Total cycles: %" PRIu64 "\n", total_cycles);
    printf("Avg cycles per FFT: %" PRIu64 "\n", avg_cycles);

	
	// cleanup opzionale (tanto poi non usciamo da main)
	m3_FreeRuntime(rt);
	m3_FreeEnvironment(env);
}

void main(void) {

    run_wasm_fft_benchmark(); 
    
}
