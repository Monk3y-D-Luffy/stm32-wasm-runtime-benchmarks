#include <zephyr/kernel.h>
#include <stm32f7xx.h>
#include "twiddle1024.h"

#define N_FFT     1024
#define NUM_ITER  100

static float buf[2 * N_FFT];

volatile uint32_t total_cycles = 0;
volatile uint32_t avg_cycles   = 0;


void enable_caches(void) {
    SCB_EnableICache();
    SCB_EnableDCache();
}

void enable_prefetch(void) {
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
}

static void fft_init(void)
{
    for (int i = 0; i < N_FFT; ++i) {
        float x = (float)i;
        buf[2 * i + 0] = x;
        buf[2 * i + 1] = 0.5f * x;
    }
}

static void bit_reverse(float *b)
{
    int j = 0;
    for (int i = 0; i < N_FFT; ++i) {
        if (i < j) {
            float tr = b[2*i+0];
            float ti = b[2*i+1];
            b[2*i+0]  = b[2*j+0];
            b[2*i+1]  = b[2*j+1];
            b[2*j+0]  = tr;
            b[2*j+1]  = ti;
        }
        int bit = N_FFT >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j |= bit;
    }
}

static void fft_radix2(float *b)
{
    bit_reverse(b);

    for (int len = 2; len <= N_FFT; len <<= 1) {
        int half_len = len >> 1;
        int stride   = N_FFT / len;

        for (int i = 0; i < N_FFT; i += len) {
            for (int j = 0; j < half_len; j++) {
                int idx1 = i + j;
                int idx2 = idx1 + half_len;
                int k    = j * stride;

                float wr = twiddle_cos[k];
                float wi = twiddle_sin[k];

                float xr = b[2*idx2+0];
                float xi = b[2*idx2+1];

                float tr = wr*xr - wi*xi;
                float ti = wr*xi + wi*xr;

                float ur = b[2*idx1+0];
                float ui = b[2*idx1+1];

                b[2*idx1+0] = ur + tr;
                b[2*idx1+1] = ui + ti;
                b[2*idx2+0] = ur - tr;
                b[2*idx2+1] = ui - ti;
            }
        }
    }
}


void fft_bench(int iterations)
{
    for (int k = 0; k < iterations; ++k) {
        fft_radix2(buf);
        
    }
}

static void DWT_Init(void)
{
    // Sblocca il DWT (richiesto su Cortex-M7 / F7)
    DWT->LAR = 0xC5ACCE55;                    // magic unlock key [web:315271]

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  // abilita blocco trace
    DWT->CYCCNT = 0;
    DWT->CTRL   |= DWT_CTRL_CYCCNTENA_Msk;           // abilita CYCCNT
}

void run_benchmark(void)
{
    DWT_Init();
    fft_init();

    uint32_t start = DWT->CYCCNT;
    
    fft_bench(NUM_ITER);
    uint32_t end = DWT->CYCCNT;
    

    total_cycles = end - start;
    avg_cycles   = total_cycles / NUM_ITER;
}

void main(void)
{
    enable_caches();
    enable_prefetch();

    //SysTick->CTRL = 0;   // disabilita SysTick
	//__disable_irq();     // opzionale se vuoi togliere tutte le IRQ

    run_benchmark();

    printk("Zephyr\r\n");
	printk("Total cycles: %lu\r\n", (unsigned long)total_cycles);
	printk("Avg cycles per FFT: %lu\r\n", (unsigned long)avg_cycles);
//printk("SystemCoreClock = %u Hz\n", SystemCoreClock);

    while (1) {
        __NOP();
    }

}