#include <stdint.h>


#define N_FFT 1024


#if defined(__wasm__) || defined(__wasm)
#  define WASM_EXPORT(name) __attribute__((export_name(name)))
#else
#  define WASM_EXPORT(name)
#endif

static float buf[2 * N_FFT];

/* ---------------------------------------------------------
 * Twiddle table (cos,sin) precomputata offline per N=1024
 * --------------------------------------------------------- */
#include "twiddle1024.h"   /* contiene float twiddle_cos[512], twiddle_sin[512] */

/* ---------------------------------------------------------
 * Init buffer
 * --------------------------------------------------------- */
WASM_EXPORT("fft_init")
void fft_init(void)
{
    for (int i = 0; i < N_FFT; ++i) {
        float x = (float)i;
        buf[2 * i + 0] = x;
        buf[2 * i + 1] = 0.5f * x;
    }
}

/* ---------------------------------------------------------
 * Bit reversal
 * --------------------------------------------------------- */
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

/* ---------------------------------------------------------
 * Radix-2 FFT
 * --------------------------------------------------------- */
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

/* ---------------------------------------------------------
 * API esportate per host WSAM3 / WAMR
 * --------------------------------------------------------- */



WASM_EXPORT("fft_bench")
void fft_bench(int iterations)
{
    for (int k = 0; k < iterations; ++k) {
        fft_radix2(buf);
        
    }
}

