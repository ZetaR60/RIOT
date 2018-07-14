/*
 * Copyright (C) 2014 Loci Controls Inc.
 *               2016 Freie Universität Berlin
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     cpu_cc2538
 * @ingroup     drivers_periph_gpio
 * @{
 *
 * @file
 * @brief       Low-level GPIO driver implementation
 *
 * @author      Ian Martin <ian@locicontrols.com>
 * @author      Hauke Petersen <hauke.petersen@fu-berlin.de>
 * @author      Sebastian Meiling <s@mlng.net>
 * @}
 */

#include <stdint.h>

#include "cpu.h"
#include "periph/gpio.h"

#define ENABLE_DEBUG (0)
#include "debug.h"

#define MODE_NOTSUP         (0xff)

static gpio_isr_ctx_t isr_ctx[4][8];

/**
 * @brief Access GPIO low-level device
 *
 * @param[in] pin   gpio pin
 *
 * @return          pointer to gpio low level device address
 */
static inline cc2538_gpio_t *gpio(gpio_t pin)
{
    return (cc2538_gpio_t *)(pin & GPIO_PORT_MASK);
}

/**
 * @brief   Helper function to get port number for gpio pin
 *
 * @param[in] pin   gpio pin
 *
 * @return          port number of gpio pin, [0=A - 3=D]
 */
static inline uint8_t _port_num(gpio_t pin)
{
    return (uint8_t)((pin & GPIO_PORTNUM_MASK) >> GPIO_PORTNUM_SHIFT) - 1;
}

/**
 * @brief   Helper function to get pin number for gpio pin
 *
 * @param[in] pin   gpio pin
 *
 * @return          pin number of gpio pin, [0 - 7]
 */
static inline uint8_t _pin_num(gpio_t pin)
{
    return (uint8_t)(pin & GPIO_PIN_MASK);
}

/**
 * @brief   Helper function to get bit mask for gpio pin number
 *
 * @param[in] pin   gpio pin
 *
 * @return          bit mask for gpio pin number, 2^[0 - 7]
 */
static inline uint32_t _pin_mask(gpio_t pin)
{
    return (1 << (pin & GPIO_PIN_MASK));
}

/**
 * @brief   Helper function to get CC2538 gpio number from port and pin
 *
 * @param[in] pin   gpio pin
 *
 * @return          number of gpio pin, [0 - 31]
 */
static inline uint8_t _pp_num(gpio_t pin)
{
    return (uint8_t)((_port_num(pin) * GPIO_BITS_PER_PORT) + _pin_num(pin));
}

int gpio_init_ll(gpio_t pin, gpio_mode_t mode)
{
    /* check if mode is valid */
    if (mode == MODE_NOTSUP) {
        return -1;
    }

    DEBUG("GPIO %"PRIu32", PORT: %u, PIN: %u\n", (uint32_t)pin, _port_num(pin), _pin_num(pin));

    /* disable any alternate function and any eventual interrupts */
    gpio(pin)->IE &= ~_pin_mask(pin);
    gpio(pin)->AFSEL &= ~_pin_mask(pin);
    /* configure pull configuration */
    IOC->OVER[_pp_num(pin)] = mode;
    /* set pin direction */
    if (mode == GPIO_OUT) {
        gpio(pin)->DIR |= _pin_mask(pin);
    }
    else {
        gpio(pin)->DIR &= ~_pin_mask(pin);
    }

    return 0;
}

int gpio_init_int_ll(gpio_t pin, gpio_mode_t mode, gpio_flank_t flank,
                     gpio_cb_t cb, void *arg)
{
    if (gpio_init_ll(pin, mode) != 0) {
        return -1;
    }

    /* store the callback information for later: */
    isr_ctx[_port_num(pin)][_pin_num(pin)].cb  = cb;
    isr_ctx[_port_num(pin)][_pin_num(pin)].arg = arg;

    /* enable power-up interrupts for this GPIO port: */
    SYS_CTRL->IWE |= (1 << _port_num(pin));

    /* configure the active flank(s) */
    gpio(pin)->IS &= ~_pin_mask(pin);
    switch(flank) {
        case GPIO_FALLING:
            gpio(pin)->IBE &= ~_pin_mask(pin);
            gpio(pin)->IEV &= ~_pin_mask(pin);
            gpio(pin)->P_EDGE_CTRL |= (1 << _pp_num(pin));
            break;
        case GPIO_RISING:
            gpio(pin)->IBE &= ~_pin_mask(pin);
            gpio(pin)->IEV |=  _pin_mask(pin);
            gpio(pin)->P_EDGE_CTRL &= ~(1 << _pp_num(pin));
            break;
        case GPIO_BOTH:
            gpio(pin)->IBE |= _pin_mask(pin);
            break;
        default:
            return -1;
    }

    /* reset interrupt status */
    gpio(pin)->IC = _pin_mask(pin);
    gpio(pin)->PI_IEN |= (1 << _pp_num(pin));
    /* enable global interrupt for the selected GPIO port */
    NVIC_EnableIRQ(GPIO_PORT_A_IRQn + _port_num(pin));
    /* unmask pin interrupt */
    gpio(pin)->IE |= _pin_mask(pin);

    return 0;
}

void gpio_irq_enable_ll(gpio_t pin)
{
    gpio(pin)->IE |= _pin_mask(pin);
}

void gpio_irq_disable_ll(gpio_t pin)
{
    gpio(pin)->IE &= ~_pin_mask(pin);
}

int gpio_read_ll(gpio_t pin)
{
    return (int)(gpio(pin)->DATA & _pin_mask(pin));
}

void gpio_set_ll(gpio_t pin)
{
    gpio(pin)->DATA |= _pin_mask(pin);
}

void gpio_clear_ll(gpio_t pin)
{
    gpio(pin)->DATA &= ~_pin_mask(pin);
}

void gpio_toggle_ll(gpio_t pin)
{
    gpio(pin)->DATA ^= _pin_mask(pin);
}

void gpio_write_ll(gpio_t pin, int value)
{
    if (value) {
        gpio(pin)->DATA |= _pin_mask(pin);
    }
    else {
        gpio(pin)->DATA &= ~_pin_mask(pin);
    }
}

static inline void handle_isr(uint8_t port_num)
{
    cc2538_gpio_t *port  = ((cc2538_gpio_t *)GPIO_BASE) + port_num;
    uint32_t state       = port->MIS;
    port->IC             = 0x000000ff;
    port->IRQ_DETECT_ACK = (0xff << (port_num * GPIO_BITS_PER_PORT));

    for (int i = 0; i < GPIO_BITS_PER_PORT; i++) {
        if (state & (1 << i)) {
            isr_ctx[port_num][i].cb(isr_ctx[port_num][i].arg);
        }
    }

    cortexm_isr_end();
}

/** @brief Interrupt service routine for Port A */
void isr_gpioa(void)
{
    handle_isr(0);
}

/** @brief Interrupt service routine for Port B */
void isr_gpiob(void)
{
    handle_isr(1);
}

/** @brief Interrupt service routine for Port C */
void isr_gpioc(void)
{
    handle_isr(2);
}

/** @brief Interrupt service routine for Port D */
void isr_gpiod(void)
{
    handle_isr(3);
}

/* CC2538 specific add-on GPIO functions */

void gpio_init_af(gpio_t pin, uint8_t sel, uint8_t over)
{
    assert(pin != GPIO_UNDEF);

    IOC->OVER[_pp_num(pin)] = over;
    if (over != GPIO_OUT) {
        IOC->PINS[sel] = _pp_num(pin);
    }
    else {
        IOC->SEL[_pp_num(pin)] = sel;
    }
    /* enable alternative function mode */
    gpio(pin)->AFSEL |= _pin_mask(pin);
}
