#include <zephyr/kernel.h>
#include <stm32f4xx.h>

void main(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;

    GPIOA->MODER   = (GPIOA->MODER & ~(3u << (5*2))) | (1u << (5*2)); // output
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3u << (5*2))) | (3u << (5*2)); // very high

    uint32_t s = 0;

    while (1) {
        s ^= 1u;
        if (s)
            GPIOA->BSRR = (1u << 5);
        else
            GPIOA->BSRR = (1u << (5 + 16));
    }
}