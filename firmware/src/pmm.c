/* pmm.c
 *
 * GPIO orchestration for the power-management module. No file-static
 * state: the PMM peripheral itself is the source of truth, and the GPIO
 * registers cache its commanded state.
 */

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "pmm.h"
#include "../config.h"

void pmm_init(void)
{
    /* Fault input — pulled up externally on the PMM board. */
    gpio_init(PMM_FAULT_PIN);
    gpio_set_dir(PMM_FAULT_PIN, GPIO_IN);

    /* Enable + diag outputs. Initialise to safe states *before* setting
     * direction-out, so the pin never glitches HIGH between gpio_set_dir
     * and the first gpio_put. */
    gpio_init(PMM_ENABLE_PIN);
    gpio_put(PMM_ENABLE_PIN, false);
    gpio_set_dir(PMM_ENABLE_PIN, GPIO_OUT);

    gpio_init(PMM_DIAG_EN_PIN);
    gpio_put(PMM_DIAG_EN_PIN, true);    /* Enable fault reporting up-front. */
    gpio_set_dir(PMM_DIAG_EN_PIN, GPIO_OUT);
}

void pmm_enable_and_wait(void)
{
    gpio_put(PMM_ENABLE_PIN, true);
    sleep_ms(PMM_INRUSH_DELAY_MS);
}

void pmm_disable(void)
{
    gpio_put(PMM_ENABLE_PIN, false);
}

bool pmm_is_fault_active(void)
{
    return gpio_get(PMM_FAULT_PIN) == 0;
}
