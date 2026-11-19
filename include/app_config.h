/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
#ifndef LETV_APP_CONFIG_H
#define LETV_APP_CONFIG_H
/* Included by the stack's cc2530_mmio.h for CC2530_APP (board
 * BOARD_EXTERNAL): macros only, no includes. */

/* board.h requires an identity; the GPD itself sends GPD_MANUFACTURER and
 * GPD_MODEL (gpd_frame.h). */
#define BOARD_MANUFACTURER "LeTV"
#define BOARD_MODEL "LeTV_Remote_GP"
#define BOARD_MODEL_LENGTH 14u

/* LeTV 8-button keypad (diyruz/freepad HAL_BOARD_LETV), CR2032:
 * buttons on P0.0..P0.7, one pin each, active low on the port 0 internal
 * pull-ups (contact bit n is P0.n); LEDs on P1.0 (red) and P1.1 (green),
 * both active low. Nothing else is connected as far as known. */
#define BOARD_BUTTONS 0xffu
#define BOARD_LED_RED 0x01u
#define BOARD_LED_GREEN 0x02u
#define BOARD_LEDS (BOARD_LED_RED | BOARD_LED_GREEN)

#define CC2530_APP_REGISTER_LIST(X) \
    X(SOC_P0IFG, 0x89) \
    X(SOC_P0IEN, 0xab) \
    X(SOC_PICTL, 0x8c)

#endif
