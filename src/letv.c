/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* LeTV keypad board: pins, LEDs and the CR2032 battery. */
#include "letv.h"

/* SWRU191F 7.18 P2INP.PDUPx: 0 pull-up for port x; 7.10 PICTL.P0ICON
 * (falling edge), IEN1.P0IE, IRCON.P0IF. */
#define PDUP0 0x20u
#define PDUP1 0x40u
#define PDUP2 0x80u
#define P0ICON 0x01u
#define P0IE 0x20u
#define P0IF 0x20u

void letv_startup(void)
{
    MMIO_WRITE(SOC_IEN0, 0);
    MMIO_WRITE(SOC_IEN1, 0);
    MMIO_WRITE(SOC_IEN2, 0);
    /* LEDs: latches high (off) first, then outputs. P1.0 and P1.1 have no
     * pulls, so they must not float as inputs either. */
    MMIO_SET(SOC_P1, BOARD_LEDS);
    MMIO_CLEAR(SOC_P1SEL, BOARD_LEDS);
    MMIO_SET(SOC_P1DIR, BOARD_LEDS);
    /* Buttons: GPIO inputs with the internal pull-ups (P0INP 0, PDUP0 0);
     * the external circuit has none. Unused P1.2..P1.7 and P2.0..P2.2 keep
     * their reset state, inputs pulled up (as the freepad LeTV firmware
     * leaves them); P2.3/P2.4 belong to the 32-kHz crystal. */
    MMIO_CLEAR(SOC_P0SEL, BOARD_BUTTONS);
    MMIO_CLEAR(SOC_P0DIR, BOARD_BUTTONS);
    MMIO_CLEAR(SOC_P0INP, BOARD_BUTTONS);
    MMIO_CLEAR(SOC_P2INP, PDUP0 | PDUP1 | PDUP2);
    /* A press is a falling edge on its pin; it sets P0IF (IRCON.5) and
     * wakes PM2 while IEN0.EA is set during sleep. */
    MMIO_SET(SOC_PICTL, P0ICON);
    MMIO_WRITE(SOC_P0IFG, 0);
    MMIO_CLEAR(SOC_IRCON, P0IF);
    MMIO_WRITE(SOC_P0IEN, BOARD_BUTTONS);
}

/* Separate from letv_startup: clock_select_init refuses to switch with any
 * IEN0..IEN2 bit set (CLOCK_UNSUPPORTED_STATE). */
void letv_buttons_arm(void)
{
    MMIO_SET(SOC_IEN1, P0IE);
}

void letv_leds(uint8_t on)
{
    MMIO_WRITE(SOC_P1, (uint8_t)((MMIO_READ(SOC_P1) | BOARD_LEDS) & ~(on & BOARD_LEDS)));
}

uint8_t letv_leds_on(void)
{
    return (uint8_t)(~MMIO_READ(SOC_P1) & BOARD_LEDS);
}

/* n single conversions (ADCCON3, SWRU191F 12.2.10) averaged as 14-bit
 * positive codes; FFFF when a conversion misses its poll bound. */
static uint16_t adc(uint8_t command, uint8_t n)
{
    uint32_t sum = 0;
    uint16_t polls, code;
    uint8_t i, low;
    for (i = n; i; i--) {
        (void)MMIO_READ(SOC_ADCH);
        MMIO_WRITE(SOC_ADCCON3, command);
        for (polls = LETV_ADC_POLLS; !(MMIO_READ(SOC_ADCCON1) & 0x80u); )
            if (!--polls) return LETV_INVALID_U16;
        low = MMIO_READ(SOC_ADCL);
        code = (uint16_t)MMIO_READ(SOC_ADCH) << 8 | low;
        if (!(code & 0x8000u)) sum += code >> 2;
    }
    return (uint16_t)(sum / n);
}

/* A CR2032 at the end of a TX burst, 0.5 % units: linear between the
 * points of an approximate lithium coin cell curve under pulse load (flat
 * near 2.9 V for most of its life, then a steep knee), 0 % at the CC2530's
 * 2.0 V minimum. An assumption, not a measured curve. */
static const MCU_CODE uint16_t curve_mv[] = {3000u, 2900u, 2800u, 2700u, 2600u, 2400u, 2000u};
static const MCU_CODE uint8_t curve_half_percent[] = {200u, 160u, 120u, 80u, 40u, 10u, 0u};

static uint8_t percent(uint16_t mv)
{
    uint8_t i, high, low;
    uint16_t top, bottom;
    if (mv >= curve_mv[0]) return curve_half_percent[0];
    for (i = 1; i < sizeof(curve_mv) / sizeof(curve_mv[0]); i++) {
        bottom = curve_mv[i];
        if (mv > bottom) {
            top = curve_mv[i-1u];
            high = curve_half_percent[i-1u];
            low = curve_half_percent[i];
            return (uint8_t)(low + (uint16_t)(mv - bottom) * (uint8_t)(high - low) / (uint16_t)(top - bottom));
        }
    }
    return 0;
}

uint16_t letv_vdd(void)
{
    return adc(0x3fu, 1);
}

void letv_battery(gpd_values_t MCU_XDATA *v, uint16_t raw)
{
    uint16_t mv;
    if (raw > LETV_ADC_MAX) {
        v->voltage = v->percent = 0xffu;
        return;
    }
    mv = LETV_MILLIVOLTS(raw);
    v->voltage = (uint8_t)((mv+50u)/100u);
    v->percent = percent(mv);
}

void letv_measure(gpd_values_t MCU_XDATA *v)
{
    letv_battery(v, adc(0x3fu, 5));
}
