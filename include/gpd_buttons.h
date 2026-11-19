/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* Button logic of the LeTV remote GP firmware, free of hardware access so
 * the host tests run it: the Press/Release decision from two contact masks
 * and the commissioning combo. Times are Sleep Timer ticks (32768 per
 * second, 24 bits). Needs gpd_frame.h for GPD_PRESS and GPD_RELEASE. */
#ifndef GPD_BUTTONS_H
#define GPD_BUTTONS_H

#include <stdint.h>

#define GPD_TICKS_SECOND 32768UL
#define GPD_TICKS_MASK 0xffffffUL
/* Mic (button 6, P0.5) and OK/center (button 8, P0.7), and nothing else. */
#ifndef GPD_COMBO_MASK
#define GPD_COMBO_MASK 0xa0u
#endif
#define GPD_COMBO_COMMISSION_TICKS (3UL*GPD_TICKS_SECOND)
#define GPD_COMBO_DECOMMISSION_TICKS (10UL*GPD_TICKS_SECOND)
#define GPD_COMBO_STUCK_TICKS (30UL*GPD_TICKS_SECOND)
/* Green blinks with this many ticks on, as many off (250 ms). */
#define GPD_COMBO_BLINK_SHIFT 13u
#define GPD_LED_RED 0x01u
#define GPD_LED_GREEN 0x02u

/* The next frame from the contacts last sent toward the contacts closed
 * now: GPD_RELEASE when any contact opened, else GPD_PRESS when any
 * closed, else 0 (nothing to send). A release and a press in one step go
 * out as two frames, release first. */
static uint8_t gpd_step(uint8_t sent, uint8_t now)
{
    if (sent & (uint8_t)~now) return GPD_RELEASE;
    if (now & (uint8_t)~sent) return GPD_PRESS;
    return 0;
}

/* That frame's payload and the new sent mask: the remaining contacts
 * (sent & now) of a release, now for a press. */
static uint8_t gpd_step_contacts(uint8_t sent, uint8_t now)
{
    return gpd_step(sent, now) == GPD_RELEASE ? (uint8_t)(sent & now) : now;
}

#define GPD_COMBO_IDLE 0u        /* no combo yet in this session */
#define GPD_COMBO_RUNNING 1u     /* exactly GPD_COMBO_MASK held */
#define GPD_COMBO_ARMED 2u       /* one of the two released, level kept */
#define GPD_COMBO_CANCELLED 3u   /* another button, a re-press or stuck */
#define GPD_COMBO_NONE 0u
#define GPD_COMBO_COMMISSION 1u
#define GPD_COMBO_DECOMMISSION 2u

typedef struct {
    uint32_t start;   /* ticks when exactly GPD_COMBO_MASK was first held */
    uint8_t state;
    uint8_t level;    /* GPD_COMBO_NONE, _COMMISSION or _DECOMMISSION */
} gpd_combo_t;

static void gpd_combo_reset(gpd_combo_t *c)
{
    c->start = 0;
    c->state = GPD_COMBO_IDLE;
    c->level = GPD_COMBO_NONE;
}

/* One debounced sample of the contacts at now; the action to run when the
 * session ends (mask 0), else GPD_COMBO_NONE. Holding exactly the combo
 * raises the level at 3 s (commission) and 10 s (decommission); releasing
 * one of its two buttons keeps the level for the release of the other. Any
 * other button, pressing the combo again after a partial release or
 * holding it 30 s cancels until every button is released. */
static uint8_t gpd_combo(gpd_combo_t *c, uint8_t mask, uint32_t now)
{
    uint32_t held;
    uint8_t action;
    if (!mask) {
        action = c->state == GPD_COMBO_CANCELLED ? GPD_COMBO_NONE : c->level;
        gpd_combo_reset(c);
        return action;
    }
    if (c->state == GPD_COMBO_CANCELLED) return GPD_COMBO_NONE;
    if (mask & (uint8_t)~GPD_COMBO_MASK) {
        c->state = GPD_COMBO_CANCELLED;
    } else if (mask == GPD_COMBO_MASK) {
        if (c->state == GPD_COMBO_IDLE) {
            c->state = GPD_COMBO_RUNNING;
            c->start = now & GPD_TICKS_MASK;
        } else if (c->state == GPD_COMBO_ARMED) {
            c->state = GPD_COMBO_CANCELLED;
        }
    } else if (c->state == GPD_COMBO_RUNNING) {
        c->state = GPD_COMBO_ARMED;
    }
    if (c->state == GPD_COMBO_RUNNING) {
        held = (now - c->start) & GPD_TICKS_MASK;
        if (held >= GPD_COMBO_STUCK_TICKS) c->state = GPD_COMBO_CANCELLED;
        else if (held >= GPD_COMBO_DECOMMISSION_TICKS) c->level = GPD_COMBO_DECOMMISSION;
        else if (held >= GPD_COMBO_COMMISSION_TICKS) c->level = GPD_COMBO_COMMISSION;
    }
    if (c->state == GPD_COMBO_CANCELLED) c->level = GPD_COMBO_NONE;
    return GPD_COMBO_NONE;
}

/* The LEDs for the combo at now: green blinking (on first) at the
 * commission level, red at the decommission level, else none. */
static uint8_t gpd_combo_leds(const gpd_combo_t *c, uint32_t now)
{
    if (c->level == GPD_COMBO_DECOMMISSION) return GPD_LED_RED;
    if (c->level == GPD_COMBO_COMMISSION)
        return ((now - c->start) & GPD_TICKS_MASK) >> GPD_COMBO_BLINK_SHIFT & 1u ? 0u : GPD_LED_GREEN;
    return 0;
}

#endif
