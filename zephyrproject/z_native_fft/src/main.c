#include <zephyr/kernel.h>
#include <stm32f4xx.h>

#define N_FFT     1024
#define NUM_ITER  100

static float buf[2 * N_FFT];
volatile uint32_t total_cycles = 0;
volatile uint32_t avg_cycles   = 0;

static void init_buffer(void) {
	for (int i = 0; i < N_FFT; ++i) {
		float x = (float)i;
		buf[2 * i + 0] = x;        // re
		buf[2 * i + 1] = 0.5f * x; // im
	}
}

static void fft_like_step(void) {
	for (int i = 0; i < N_FFT; i += 2) {
		int ia = i;
		int ib = i + 1;

		float a_re = buf[2 * ia + 0];
		float a_im = buf[2 * ia + 1];
		float b_re = buf[2 * ib + 0];
		float b_im = buf[2 * ib + 1];

		float u_re = a_re + b_re;
		float u_im = a_im + b_im;
		float v_re = a_re - b_re;
		float v_im = a_im - b_im;

		// simulazione twiddle / scaling
		u_re *= 0.5f;
		u_im *= 0.5f;
		v_re *= 0.5f;
		v_im *= 0.5f;

		buf[2 * ia + 0] = u_re;
		buf[2 * ia + 1] = u_im;
		buf[2 * ib + 0] = v_re;
		buf[2 * ib + 1] = v_im;
	}
}

static void bench(int iterations) {
	for (int k = 0; k < iterations; ++k) {
		fft_like_step();
	}
}


//------------------------------
// DWT cycle counter
//------------------------------
static void DWT_Init(void) {
	// abilita il blocco di trace
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	// azzera il contatore
	DWT->CYCCNT = 0;
	// abilita il contatore di cicli
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void run_benchmark(void) {
    DWT_Init();
    init_buffer();

    uint32_t start = DWT->CYCCNT;
    bench(NUM_ITER);
    uint32_t end   = DWT->CYCCNT;

    total_cycles = end - start;
    avg_cycles   = total_cycles / NUM_ITER;
}

void main(void)
{
   SysTick->CTRL = 0;   // disabilita SysTick
	__disable_irq();     // opzionale se vuoi togliere tutte le IRQ

	/* USER CODE END 2 */

	/* Infinite loop */
	/* USER CODE BEGIN WHILE */

	run_benchmark();

	printk("Zephyr\r\n");
	printk("Total cycles: %lu\r\n", (unsigned long)total_cycles);
	printk("Avg cycles per FFT: %lu\r\n", (unsigned long)avg_cycles);
//printk("SystemCoreClock = %u Hz\n", SystemCoreClock);

	while (1)
	{
		/* USER CODE END WHILE */

		/* USER CODE BEGIN 3 */
		__NOP(); // metti un breakpoint e guarda total_cycles / avg_cycles
	}
}