#include <zephyr/kernel.h>
#include <stm32f7xx.h>

#include <arm_cmse.h> // o core_cm7.h
#include <core_cm7.h>
#include "stm32f7xx_hal.h"  // o h7xx

void enable_caches(void) {
    SCB_EnableICache();
    SCB_EnableDCache();
}

void enable_prefetch(void) {
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
}

void main(void)
{

    enable_caches();
    enable_prefetch();
    
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;

    GPIOA->MODER   = (GPIOA->MODER & ~(3u << (5*2))) | (1u << (5*2)); // output
    GPIOA->OSPEEDR = (GPIOA->OSPEEDR & ~(3u << (5*2))) | (3u << (5*2)); // very high
    printk("SYSCLK = %lu\n", SystemCoreClock);
    uint32_t s = 0;

    while (1) {
        s ^= 1u;
        if (s)
            GPIOA->BSRR = (1u << 5);
        else
            GPIOA->BSRR = (1u << (5 + 16));
    //for (volatile int i=0; i<6000000; i++);
    }
}