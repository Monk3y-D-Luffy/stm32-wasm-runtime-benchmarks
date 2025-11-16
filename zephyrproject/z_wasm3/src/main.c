#include <zephyr/kernel.h>
#include <stm32f4xx.h>
#include "wasm3.h"
#include "toggle.wasm.h"

m3ApiRawFunction(host_gpio_toggle) {
    static uint32_t s;
    s ^= 1u;
    if (s) GPIOA->BSRR = (1u << 5);
    else   GPIOA->BSRR = (1u << (5 + 16));
    m3ApiSuccess();
}

static inline void link_host(IM3Module mod) {
    m3_LinkRawFunction(mod, "env", "gpio_toggle", "v()", &host_gpio_toggle);
}

static void run_wasm_toggle_forever(void) {
    IM3Environment env = m3_NewEnvironment();
    if (!env) return;

    IM3Runtime rt = m3_NewRuntime(env, 8*1024, NULL);
    if (!rt) return;

    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod,
                     (const uint8_t*)toggle_wasm, toggle_wasm_len);
    if (r) return;

    r = m3_LoadModule(rt, mod);
    if (r) return;

    link_host(mod);

    IM3Function f = NULL;
    r = m3_FindFunction(&f, rt, "toggle_forever");
    if (r || !f) return;

    (void)m3_CallV(f);   // non ritorna finché la VM gira
}

static inline void gpio_pa5_init(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIOA->MODER   = (GPIOA->MODER & ~(3u << (5*2))) | (1u << (5*2));
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3u << (5*2))) | (3u << (5*2));
}

void main(void) {
    gpio_pa5_init();
    run_wasm_toggle_forever();
    while (1) {
        /* se mai tornasse, non fare altro */
    }
}
