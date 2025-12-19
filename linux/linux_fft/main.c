#include "twiddle1024.h"
#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

#define N_FFT     1024
#define NUM_ITER  100

static float buf[2 * N_FFT];

uint64_t cpu_hz = 3700000000ULL; // 3.7 GHz
uint64_t total_ns = 0;
uint64_t total_cycles = 0;
uint64_t avg_cycles = 0;

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

static uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

void run_benchmark(void)
{
	fft_init();

	uint64_t start = ns_now();
	fft_bench(NUM_ITER);
	uint64_t end = ns_now();

	total_ns = end - start;
	// total_ns in ns, cpu_hz in Hz
	total_cycles = total_ns * (cpu_hz / 1000000000ULL);
	avg_cycles   = total_cycles / NUM_ITER;
	
}

int main(void)
{
	run_benchmark();
	printf("Linux\r\n");
	printf("Total cycles: %" PRIu64 "\n", total_cycles);
    printf("Avg cycles per FFT: %" PRIu64 "\n", avg_cycles);

}
