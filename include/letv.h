/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
#ifndef LETV_H
#define LETV_H
/* LeTV keypad board: startup pin state, LEDs, buttons and battery. */
#include "board.h"
#include "gpd_frame.h"

#define LETV_ADC_POLLS 4000u
#define LETV_INVALID_U16 0xffffu
/* CC2530 single-ended 14-bit positive half range. */
#define LETV_ADC_MAX 8191u
/* mV = raw * 0.443 (VDD/3, internal 1.15 V reference). */
#define LETV_MILLIVOLTS(raw) ((uint16_t)(((uint32_t)(raw) * 443u) / 1000u))

/* The pressed buttons, bit n for P0.n (active low inputs). */
#define LETV_BUTTONS() ((uint8_t)~MMIO_READ(SOC_P0))

/* Interrupts off, LEDs off, the button inputs on their pull-ups and the
 * port 0 falling-edge interrupt armed (IEN1.P0IE, not yet IEN0.EA): the
 * first thing main does. */
void letv_startup(void);
/* LEDs: BOARD_LED_RED and/or BOARD_LED_GREEN on, all others off. */
void letv_leds(uint8_t on);
/* The LEDs that are on. */
uint8_t letv_leds_on(void);
/* Battery into v from an idle average of five conversions. */
void letv_measure(gpd_values_t MCU_XDATA *v);
/* One raw VDD/3 conversion (internal reference), LETV_INVALID_U16 on a fault. */
uint16_t letv_vdd(void);
/* Battery voltage and percentage of v from a raw VDD/3 average. */
void letv_battery(gpd_values_t MCU_XDATA *v, uint16_t raw);

#endif
