/*
 * Copyright (c) 2022, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: weilin.peng@artinchip.com
 */

#include <aic_core.h>
#include <aic_hal.h>
#include "board.h"

struct aic_pinmux
{
    unsigned char       func;
    unsigned char       bias;
    unsigned char       drive;
    char *              name;
};

/*
 * AKP05 board pinmux.
 * Recovered from the stock firmware v1.2.7 (table at file offset 0xa4790).
 * func 5 = UART0, 4 = I2C, 2 = display/USB, 3 = PWM2.
 */
struct aic_pinmux aic_pinmux_config[] = {
    {5, PIN_PULL_DIS, 3, "PA.0"},
    {5, PIN_PULL_UP, 3, "PA.1"},
    {4, PIN_PULL_DIS, 3, "PA.8"},
    {4, PIN_PULL_DIS, 3, "PA.9"},
    {2, PIN_PULL_DIS, 3, "PE.14"},
    {2, PIN_PULL_DIS, 3, "PD.0"},
    {2, PIN_PULL_DIS, 3, "PD.1"},
    {2, PIN_PULL_DIS, 3, "PD.2"},
    {2, PIN_PULL_DIS, 3, "PD.3"},
    {2, PIN_PULL_DIS, 3, "PD.4"},
    {2, PIN_PULL_DIS, 3, "PD.5"},
    {2, PIN_PULL_DIS, 3, "PD.6"},
    {2, PIN_PULL_DIS, 3, "PD.7"},
    {2, PIN_PULL_DIS, 3, "PD.8"},
    {2, PIN_PULL_DIS, 3, "PD.9"},
    {2, PIN_PULL_DIS, 3, "PD.10"},
    {2, PIN_PULL_DIS, 3, "PD.11"},
    {2, PIN_PULL_DIS, 3, "PD.12"},
    {2, PIN_PULL_DIS, 3, "PD.13"},
    {2, PIN_PULL_DIS, 3, "PD.14"},
    {2, PIN_PULL_DIS, 3, "PD.15"},
    {2, PIN_PULL_DIS, 3, "PD.16"},
    {2, PIN_PULL_DIS, 3, "PD.17"},
    {2, PIN_PULL_DIS, 3, "PD.18"},
    {2, PIN_PULL_DIS, 3, "PD.19"},
    {2, PIN_PULL_DIS, 3, "PD.20"},
    {2, PIN_PULL_DIS, 3, "PD.21"},
    {2, PIN_PULL_DIS, 3, "PD.22"},
    {2, PIN_PULL_DIS, 3, "PD.23"},
    {2, PIN_PULL_DIS, 3, "PD.24"},
    {2, PIN_PULL_DIS, 3, "PD.25"},
    {2, PIN_PULL_DIS, 3, "PD.26"},
    {2, PIN_PULL_DIS, 3, "PD.27"},
    {3, PIN_PULL_DIS, 3, "PE.13"},
    {2, PIN_PULL_DIS, 3, "PO.0"},
    {2, PIN_PULL_DIS, 3, "PO.1"},
};

void aic_board_pinmux_init(void)
{
    uint32_t i = 0;
    long pin = 0;
    unsigned int g;
    unsigned int p;

    for (i=0; i<ARRAY_SIZE(aic_pinmux_config); i++) {
        pin = hal_gpio_name2pin(aic_pinmux_config[i].name);
        if (pin < 0)
            continue;
        g = GPIO_GROUP(pin);
        p = GPIO_GROUP_PIN(pin);
        hal_gpio_set_func(g, p, aic_pinmux_config[i].func);
        hal_gpio_set_bias_pull(g, p, aic_pinmux_config[i].bias);
        hal_gpio_set_drive_strength(g, p, aic_pinmux_config[i].drive);
    }
}
