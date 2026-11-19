/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* LeTV 8-button remote as a Green Power Device (Generic Switch). No Zigbee
 * join, no stack runtime (only the cc2530-zigbee clock, Sleep Timer and AES
 * helpers): GPDFs (include/gpd_frame.h, unidirectional commissioning) on
 * one channel.
 *
 * Security (GPD_SECURITY, default): SecurityLevel 3 with an individual key,
 * generated from radio noise on the first commissioning and kept in flash
 * until a decommissioning or a chip erase. The Commissioning GPDF carries
 * it protected with the default TC link key, as the ZHA (zigpy) and
 * Zigbee2MQTT (zigbee-herdsman) sinks expect; every other GPDF but the
 * Channel Requests is encrypted and authenticated with it. The security
 * frame counter never repeats: flash holds a lease of 1024 counters, taken
 * before its first counter is used, and a restart continues after it.
 *
 * Channel: with no saved channel (first start), settings of another
 * firmware, a power-on or RESET_N reset (GPD_RESET_COMMISSION: a battery
 * insertion or a debugger reset; a sink without an open commissioning
 * window ignores the frames) or the 3-s combo the device commissions:
 * each of up to
 * GPD_COMMISSION_ROUNDS rounds (GPD_COMMISSION_PERIOD apart) first searches
 * the channel with GP Channel Requests (A.3.9.1: Auto-Commissioning 1 on
 * every channel, then 0 on the hint channel and a receive window there; a
 * GP proxy of the network, acting as TempMaster while the sink's
 * commissioning window, ZHA permit join, is open, answers with a Channel
 * Configuration on the hint channel). An answer ends the rounds: one
 * Commissioning GPDF and a battery report on that channel, which is saved
 * in flash (settings.h). Without an answer (no proxy, or permit join still
 * closed) the round sends them on the hint channel (the saved one or
 * GPD_CHANNEL) instead.
 *
 * Buttons (P0.0..P0.7, active low, any press wakes PM2 through the port 0
 * interrupt): after a 20-ms debounce the contacts are sampled every 20 ms
 * (PM2 between samples) while any is closed; a sample equal to the one
 * before is a debounced state, and every change goes out at once as an
 * 8-bit vector Press (contacts added) or Release (contacts removed) with
 * the contacts closed now. Contacts unchanged for 30 s are sampled once a
 * second instead (a new press still wakes at once). Mic + OK held alone
 * (gpd_buttons.h): green blinks from 3 s, red from 10 s; on release 3 s
 * commissions, 10 s sends a Decommissioning GPDF, clears the settings and
 * stops sending until the next commissioning.
 *
 * Each GPDF goes out GPD_REPEAT times with the same MAC sequence number and
 * security frame counter (the sink drops the duplicates), GPD_REPEAT_GAP
 * apart. A Battery report (Power Configuration attributes) follows each
 * commissioning and every GPD_HEARTBEAT_WAKES Sleep Timer wakes,
 * GPD_HEARTBEAT_CHAIN seconds apart (6 h); its values come from VDD at the
 * end of the transmissions of the GPDF sent before. Any bounded wait that
 * expires restarts the device through the watchdog.
 *
 * LEDs: a 20-ms green flash per Press (GPD_PRESS_FLASH), a green flash per
 * channel search attempt, green 1 s when a channel was found, three red
 * blinks when not, five red blinks after a decommissioning. */
#include "letv.h"
#include "gpd_buttons.h"
#include "settings.h"
#include "clock.h"
#include "timebase.h"
#if GPD_SECURITY
#include "aes.h"
#endif

#if BOARD_LED_RED != GPD_LED_RED || BOARD_LED_GREEN != GPD_LED_GREEN
#error "gpd_buttons.h LED bits differ from the board"
#endif

/* The hint: the channel searched first and used when no proxy answers. */
#ifndef GPD_CHANNEL
#define GPD_CHANNEL 15u
#endif
#ifndef GPD_COMMISSION_PERIOD
#define GPD_COMMISSION_PERIOD 10u
#endif
#ifndef GPD_COMMISSION_ROUNDS
#define GPD_COMMISSION_ROUNDS 12u
#endif
/* Channel search attempts per round (the TempMaster needs one attempt to
 * be told by the sink, then waits up to 5 s on the hint channel) and the
 * Sleep Timer ticks between them. */
#ifndef GPD_SEARCH_ATTEMPTS
#define GPD_SEARCH_ATTEMPTS 6u
#endif
#ifndef GPD_SEARCH_GAP
#define GPD_SEARCH_GAP 13107UL     /* 400 ms */
#endif
#ifndef GPD_REPEAT
#define GPD_REPEAT 3u
#endif
/* Sleep Timer ticks between repetitions: 655 is 20 ms. Three
 * transmissions of a secured Press (23 bytes, about 1.4 ms on air with the
 * calibration) end about 45 ms after the first started. */
#ifndef GPD_REPEAT_GAP
#define GPD_REPEAT_GAP 655UL
#endif
/* Sleep Timer wakes for the battery heartbeat: the 24-bit timer wraps after
 * 512 s and a deadline must stay within half of that, so the heartbeat is a
 * chain of GPD_HEARTBEAT_CHAIN-second wakes, every GPD_HEARTBEAT_WAKES-th
 * sending a report (240 s * 90 = 6 h). */
#ifndef GPD_HEARTBEAT_CHAIN
#define GPD_HEARTBEAT_CHAIN 240u
#endif
#ifndef GPD_HEARTBEAT_WAKES
#define GPD_HEARTBEAT_WAKES 90u
#endif
#if GPD_HEARTBEAT_CHAIN > 250u
#error "GPD_HEARTBEAT_CHAIN must stay below half the 512-s Sleep Timer period"
#endif
/* Commission after a power-on or RESET_N reset even when paired, so the
 * remote pairs again without its buttons (a watchdog restart does not). */
#ifndef GPD_RESET_COMMISSION
#define GPD_RESET_COMMISSION 1
#endif
/* A 20-ms green flash with the first transmission of each Press. */
#ifndef GPD_PRESS_FLASH
#define GPD_PRESS_FLASH 1
#endif
/* SWRS081B Table 2: F5 is +4.5 dBm (about 34 mA in TX). */
#ifndef GPD_TXPOWER
#define GPD_TXPOWER 0xf5u
#endif

/* SWRU191F 16.3 WDCTL, 4.10 SLEEPSTA, 11.2 STLOAD. */
#define SOC_WDCTL_ADDRESS 0xc9u
#define SOC_SLEEPSTA_ADDRESS 0x9du
#define SOC_STLOAD_ADDRESS 0xadu
#define WDCTL_RESET 0x0bu
/* SLEEPSTA.RST: 00 power-on or brown-out, 01 RESET_N, 10 watchdog, 11 clock loss. */
#define SLEEPSTA_RST_MASK 0x18u
#define SLEEPSTA_RST_WATCHDOG 0x10u
/* Factory IEEE address in the information page, LSB first (SWRU191F 2.3.3). */
#define INFO_IEEE 0x780cu
/* SWRU191F 23.15 command strobes, RFIRQF0.RXPKTDONE, RFIRQF1.TXDONE,
 * FRMFILT0 (frame filtering off), RXFIFOCNT. */
#define RFST_ISRXON 0xe3u
#define RFST_ISTXON 0xe9u
#define RFST_ISFLUSHRX 0xedu
#define RFST_ISFLUSHTX 0xeeu
#define RFST_ISRFOFF 0xefu
#define RXPKTDONE 0x40u
#define TXDONE 0x02u
#define FRMFILT0 0x6180u
#define RXFIFOCNT 0x619bu
/* SWRU191F 23.12, 23.15: RSSISTAT.RSSI_VALID, RFRND.IRND. */
#define RSSISTAT 0x6199u
#define RFRND 0x61a7u
#define FREQCTRL 0x618fu
/* One AES block: about 10 us; generous bounds, a failure restarts. */
#define AES_WAIT 328UL     /* 10 ms */
#define AES_POLLS 4000u
/* SWRU191F 7.10 IRCON.P0IF. */
#define P0IF 0x20u
#define SECOND GPD_TICKS_SECOND
#define TX_WAIT 164UL      /* 5 ms per GPDF (about 1.6 ms on air) */
#define BLINK 4915UL       /* 150 ms */
#define SPIN_WAIT 4096UL   /* 125 ms for clock and Sleep Timer handshakes */
/* Receive window after the start of the last Channel Request: the
 * TempMaster answers 20..25 ms after it (gpTxOffset, gpMaxTxOffsetVariation)
 * within gpTxDuration (10 ms). */
#define RX_OPEN 590UL      /* 18 ms */
#define RX_CLOSE 1311UL    /* 40 ms */
#define RX_FRAME 164UL     /* 5 ms for a frame being received to complete */
#define BUTTON_POLL 655UL               /* 20 ms: debounce and sample period */
#define BUTTON_SLOW SECOND
#define BUTTON_STUCK (30UL*SECOND)
#define SLEEP_MIN 8UL
/* SWRU191F 4.1 SLEEPCMD.MODE with OSC_PD (unused 16-MHz RCOSC off):
 * PM2, or PM0 where PCON.IDLE only halts the CPU. */
#define SLEEP_PM2 0x06u
#define SLEEP_IDLE 0x04u

/* Read by a debugger: progress and the last failure. */
typedef struct {
    uint8_t stage, failure, rounds, seq;
    uint16_t sent, lost;
    uint8_t src[4];
    uint8_t channel, found, contacts, heartbeats;
    uint16_t presses;
} gpd_status_t;
MCU_XDATA gpd_status_t gpd_status;

static MCU_XDATA uint8_t frame[GPD_FRAME_LENGTH], ieee[8], rx[128];
static MCU_XDATA uint8_t paired;
/* sleep_until with watch returns for a contact closed that is not in held. */
static MCU_XDATA uint8_t held;
static MCU_XDATA gpd_values_t values;
static MCU_XDATA settings_t settings;
static MCU_XDATA clock_diagnostics_t clock_diagnostics;
static MCU_XDATA gpd_combo_t combo;
static MCU_XDATA uint32_t deadline, mark;
static volatile MCU_XDATA uint16_t polls;
/* Sum of the raw VDD/3 readings taken at the end of each transmission of
 * the GPDF being sent, and their count. */
static MCU_XDATA uint16_t loaded;
static MCU_XDATA uint8_t loads;
/* Heartbeat chain wakes since the last battery report. */
static MCU_XDATA uint8_t chain;
#if GPD_SECURITY
/* The next security frame counter. */
static MCU_XDATA uint32_t counter;
static MCU_XDATA aes_diagnostics_t aes_diagnostics;
#endif
#define S gpd_status
#define NOW() timebase_read_awake_ticks24()
#define SINCE(start) ((NOW()-(start))&TIMEBASE_TICKS_MASK)
#define LATER(ticks) ((NOW()+(ticks))&TIMEBASE_TICKS_MASK)

/* Never taken in a correct sequence: the instruction after the wake clears
 * EA before the vector can run (SWRU191F 4.2). Leaves that only clear the
 * Sleep Timer flag (STIF) and the button flags (P0IFG, then IRCON.P0IF). */
__attribute__((interrupt(5))) MCU_COMMON_CODE void gpd_sleep_isr(void)
{
    __asm__ volatile("clr 0xc7");
}

__attribute__((interrupt(13))) MCU_COMMON_CODE void gpd_p0_isr(void)
{
    __asm__ volatile("mov 0x89,#0\n\tclr 0xc5");
}

__attribute__((noinline)) static void restart(uint8_t failure)
{
    S.failure = failure;
    MMIO_CLEAR(SOC_IEN0, 0x80);
    MMIO_WRITE(SOC_RFST, RFST_ISRFOFF);
    letv_leds(0);
    MMIO_WRITE(SOC_WDCTL, WDCTL_RESET);
    for (;;) {}
}

/* A contact closed that is not one of held (the level, checked after every
 * wake: an edge between this check and the sleep is still caught by the
 * flag, one before the flags were cleared by this check). */
static uint8_t pressed(void)
{
    return (LETV_BUTTONS() & (uint8_t)~held) != 0;
}

/* SWRU191F 23.15: the cc2530-zigbee radio profile (radio_autoack.c),
 * AUTOCRC without AUTOACK, no frame filtering, then channel and power. */
static void radio_setup(uint8_t channel)
{
    MMIO_WRITE(SOC_RFST, RFST_ISRFOFF);
    MMIO_XWRITE(FRMFILT0, 0x0c);
    MMIO_XWRITE(0x6189u, 0x40);   /* FRMCTRL0 */
    MMIO_XWRITE(0x61b2u, 0x15);   /* AGCCTRL1 */
    MMIO_XWRITE(0x61fau, 0x09);   /* TXFILTCFG */
    MMIO_XWRITE(0x61aeu, 0x00);   /* FSCAL1 */
    MMIO_XWRITE(0x6191u, 0x69);   /* TXCTRL */
    MMIO_XWRITE(0x618fu, 11u + 5u*(channel-11u));   /* FREQCTRL */
    MMIO_XWRITE(0x6190u, GPD_TXPOWER);
}

/* STIF clear; IEN0 = EA|STIE, then PCON.IDLE whose successor must not start
 * on a 4-byte boundary (SWRU191F 4.2); clr EA runs before any ISR. */
__attribute__((noinline)) MCU_COMMON_CODE static void enter_pm2(void)
{
    __asm__ volatile("clr 0xc7\n\t.p2align 2\n\tmov 0xa8,#0xa0\n\tmov 0x87,#1\n\tclr 0xaf" ::: "memory");
}

#define SPIN(condition, code) do { mark = NOW(); \
    while (condition) if (SINCE(mark) >= SPIN_WAIT) restart(code); } while (0)

/* Until a positive 32-kHz edge after a PM2 wake the Sleep Timer still reads
 * its value from before the sleep (SWRU191F 11.4), so a SPIN deadline there
 * expired at once after a 10 s or 60 s sleep and restarted every report.
 * These waits are bounded by a poll count instead (as join_smoke). */
#define POLL(condition, code) do { polls = 0; \
    while (condition) if (!++polls) restart(code); } while (0)

#if GPD_SECURITY
/* Interrupts are off outside sleep_until (IEN0 = 0). */
void gpd_aes(const uint8_t MCU_XDATA *key, uint8_t MCU_XDATA *block)
{
    if (aes128_encrypt_block((const MCU_GENERIC uint8_t *)key, (const MCU_GENERIC uint8_t *)block, block,
                             AES_WAIT, AES_POLLS, &aes_diagnostics) != AES_OK)
        restart(10);
}

/* A security frame counter, the lease extended in flash first. */
static uint32_t take(void)
{
    if ((uint16_t)(counter >> 10) >= settings.lease) {
        settings.lease = (uint16_t)((counter >> 10) + 1u);
        settings_save(&settings);
    }
    return counter++;
}

/* A new key (SWRU191F 23.12): receiver on below the band (FREQCTRL 0, as
 * the guide's procedure) until RSSI is valid, 1024 RFRND.IRND bits XORed
 * into 16 bytes, which then key AES over the IEEE address and Sleep Timer
 * as a whitening step. */
static void generate_key(void)
{
    uint16_t i;
    uint8_t MCU_XDATA *pool = rx, MCU_XDATA *block = rx + 16;
    radio_setup(GPD_CHANNEL);
    MMIO_XWRITE(FREQCTRL, 0);
    for (i = 0; i < 32u; i++) rx[i] = 0;
    MMIO_WRITE(SOC_RFST, RFST_ISRXON);
    SPIN(!(MMIO_XREAD(RSSISTAT) & 1u), 9);
    for (i = 0; i < 1024u; i++)
        pool[(i >> 3) & 15u] ^= (uint8_t)((MMIO_XREAD(RFRND) & 1u) << (i & 7u));
    MMIO_WRITE(SOC_RFST, RFST_ISRFOFF);
    for (i = 0; i < 8u; i++) block[i] = ieee[i];
    block[8] = MMIO_READ(SOC_ST0); block[9] = MMIO_READ(SOC_ST1); block[10] = MMIO_READ(SOC_ST2);
    block[11] = S.seq;
    gpd_aes(pool, block);
    for (i = 0; i < 16u; i++) settings.key[i] = block[i];
    settings.keyed = 0xffu;
    settings_save(&settings);
}
#define SECURE(n) gpd_secure(frame, (n), settings.key, take())
#else
#define SECURE(n) (n)
#endif

/* PM2 (or CPU idle) until the Sleep Timer reaches wake (as cc2530-zigbee
 * join_smoke_sleep: compare ST2, ST1, ST0 after STLOAD; after PM2 the XOSC
 * back, then a fresh 32-kHz edge). A wake too close ends in a busy wait; a
 * port 0 wake sleeps again, or with watch returns 1 for a new contact
 * (pressed). */
static uint8_t sleep_until(uint32_t wake, uint8_t mode, uint8_t watch)
{
    uint32_t left;
    for (;;) {
        S.stage = 4;
        left = (wake-NOW())&TIMEBASE_TICKS_MASK;
        if (left >= TIMEBASE_TICKS_MASK/2u) return 0;
        if (watch && pressed()) return 1;
        if (left <= SLEEP_MIN*2u) continue;
        SPIN(!(MMIO_READ(SOC_STLOAD) & 1u), 5);
        MMIO_WRITE(SOC_ST2, (uint8_t)(wake >> 16));
        MMIO_WRITE(SOC_ST1, (uint8_t)(wake >> 8));
        MMIO_WRITE(SOC_ST0, (uint8_t)wake);
        if (((wake-NOW())&TIMEBASE_TICKS_MASK) < SLEEP_MIN) continue;
        MMIO_WRITE(SOC_P0IFG, 0);
        MMIO_CLEAR(SOC_IRCON, P0IF);
        if (watch && pressed()) return 1;
        MMIO_WRITE(SOC_SLEEPCMD, mode);
        enter_pm2();
        MMIO_WRITE(SOC_IEN0, 0);
        MMIO_WRITE(SOC_SLEEPCMD, SLEEP_IDLE);
        if (mode == SLEEP_PM2) {
            S.stage = 5;
            POLL(MMIO_READ(SOC_CLKCONSTA) != MMIO_READ(SOC_CLKCONCMD), 6);
            POLL(MMIO_READ(SOC_SLEEPSTA) & 1u, 7);
            POLL(!(MMIO_READ(SOC_SLEEPSTA) & 1u), 8);
        }
    }
}

static void nap(uint32_t ticks, uint8_t mode)
{
    (void)sleep_until(LATER(ticks), mode, 0);
}

static void blink(uint8_t leds, uint8_t n)
{
    while (n--) {
        letv_leds(leds); nap(BLINK, SLEEP_PM2);
        letv_leds(0); nap(BLINK, SLEEP_PM2);
    }
}

/* One GPDF: TX FIFO length (with FCS) and body, ISTXON (calibrates, then
 * sends without CCA), TXDONE, radio off. */
static void transmit(uint8_t length)
{
    uint16_t vdd, last = LETV_INVALID_U16;
    uint8_t i;
    MMIO_WRITE(SOC_RFST, RFST_ISFLUSHTX);
    MMIO_WRITE(SOC_RFD, length + 2u);
    for (i = 0; i < length; i++) MMIO_WRITE(SOC_RFD, frame[i]);
    MMIO_WRITE(SOC_RFIRQF1, (uint8_t)~TXDONE);
    MMIO_WRITE(SOC_RFST, RFST_ISTXON);
    mark = NOW();
    /* VDD sags most at the end of the frame: keep the last VDD conversion
     * (about 0.13 ms) that ends before TXDONE. */
    while (!(MMIO_READ(SOC_RFIRQF1) & TXDONE)) {
        if (SINCE(mark) >= TX_WAIT) break;
        vdd = letv_vdd();
        if (!(MMIO_READ(SOC_RFIRQF1) & TXDONE)) last = vdd;
    }
    if (MMIO_READ(SOC_RFIRQF1) & TXDONE) {
        S.sent++;
        if (last <= LETV_ADC_MAX) {
            loaded += last;
            loads++;
        }
    } else S.lost++;
    MMIO_WRITE(SOC_RFIRQF1, (uint8_t)~TXDONE);
    MMIO_WRITE(SOC_RFST, RFST_ISRFOFF);
}

/* One GPDF GPD_REPEAT times on S.channel; PM2 between repetitions, then the
 * radio profile again. flash: the green LED on for the first repetition
 * and gap. The battery values become VDD under this GPDF's TX load. */
static void send(uint8_t length, uint8_t flash)
{
    uint8_t r;
    loaded = 0;
    loads = 0;
    radio_setup(S.channel);
    if (flash) letv_leds(BOARD_LED_GREEN);
    for (r = 0; r < GPD_REPEAT; r++) {
        if (r) {
            nap(GPD_REPEAT_GAP, SLEEP_PM2);
            if (flash) { letv_leds(0); flash = 0; }
            radio_setup(S.channel);
        }
        transmit(length);
    }
    if (flash) letv_leds(0);
    if (loads) letv_battery(&values, loaded / loads);
    S.seq++;
}

/* Receive from RX_OPEN to RX_CLOSE after start: the operational channel of
 * a Channel Configuration with a good FCS, or 0. */
static uint8_t listen(uint32_t start)
{
    uint8_t found = 0, n, i;
    nap((RX_OPEN - SINCE(start)) & TIMEBASE_TICKS_MASK, SLEEP_IDLE);
    MMIO_WRITE(SOC_RFST, RFST_ISFLUSHRX);
    MMIO_WRITE(SOC_RFST, RFST_ISFLUSHRX);
    MMIO_WRITE(SOC_RFIRQF0, (uint8_t)~RXPKTDONE);
    MMIO_WRITE(SOC_RFST, RFST_ISRXON);
    while (!found && SINCE(start) < RX_CLOSE) {
        if (!(MMIO_READ(SOC_RFIRQF0) & RXPKTDONE)) continue;
        MMIO_WRITE(SOC_RFIRQF0, (uint8_t)~RXPKTDONE);
        while (!found && MMIO_XREAD(RXFIFOCNT)) {
            n = MMIO_READ(SOC_RFD) & 0x7fu;
            mark = NOW();
            while (MMIO_XREAD(RXFIFOCNT) < n)
                if (SINCE(mark) >= RX_FRAME) break;
            if (!n || MMIO_XREAD(RXFIFOCNT) < n) {
                MMIO_WRITE(SOC_RFST, RFST_ISFLUSHRX);
                MMIO_WRITE(SOC_RFST, RFST_ISFLUSHRX);
                break;
            }
            for (i = 0; i < n; i++) rx[i] = MMIO_READ(SOC_RFD);
            if (rx[n-1u] & 0x80u) found = gpd_channel_configuration(rx, n);
        }
    }
    MMIO_WRITE(SOC_RFST, RFST_ISRFOFF);
    MMIO_WRITE(SOC_RFST, RFST_ISFLUSHRX);
    MMIO_WRITE(SOC_RFST, RFST_ISFLUSHRX);
    return found;
}

/* GPD_SEARCH_ATTEMPTS Channel Request bursts announcing hint as the receive
 * channel: Auto-Commissioning 1 on every other channel, 0 on hint, then the
 * receive window there. The found channel, or 0. */
static uint8_t search(uint8_t hint)
{
    uint8_t attempt, channel, found;
    uint32_t start;
    for (attempt = 0; attempt < GPD_SEARCH_ATTEMPTS; attempt++) {
        if (attempt) nap(GPD_SEARCH_GAP, SLEEP_PM2);
        letv_leds(BOARD_LED_GREEN);
        for (channel = 11u; channel <= 26u; channel++) {
            if (channel == hint) continue;
            radio_setup(channel);
            transmit(gpd_channel_request(frame, S.seq++, 1, hint));
        }
        radio_setup(hint);
        start = NOW();
        transmit(gpd_channel_request(frame, S.seq++, 0, hint));
        found = listen(start);
        letv_leds(0);
        if (found) return found;
    }
    return 0;
}

static void report(void)
{
    send(SECURE(gpd_battery(frame, S.seq, S.src, &values)), 0);
    chain = 0;
    S.heartbeats++;
}

/* The heartbeat chain: when deadline passed, the next link (or, if that
 * is already past too, one from now) and every GPD_HEARTBEAT_WAKES-th link
 * a battery report. */
static void heartbeat(void)
{
    if (((deadline-NOW())&TIMEBASE_TICKS_MASK) < TIMEBASE_TICKS_MASK/2u) return;
    deadline = (deadline + (uint32_t)GPD_HEARTBEAT_CHAIN*SECOND) & TIMEBASE_TICKS_MASK;
    if (((deadline-NOW())&TIMEBASE_TICKS_MASK) >= TIMEBASE_TICKS_MASK/2u)
        deadline = LATER((uint32_t)GPD_HEARTBEAT_CHAIN*SECOND);
    if (++chain >= GPD_HEARTBEAT_WAKES) {
        chain = 0;
        if (paired) report();
    }
}

/* Rounds of search, then commissioning and a battery report; the channel
 * found is saved with the profile. A button press ends the rounds. */
static void commission(void)
{
    uint8_t hint = settings.channel ? settings.channel : GPD_CHANNEL, found = 0;
    uint32_t round;
#if GPD_SECURITY
    if (settings.keyed != 0xffu) generate_key();
#endif
    for (S.rounds = 0; S.rounds < GPD_COMMISSION_ROUNDS && !found; ) {
        S.rounds++;
        round = LATER((uint32_t)GPD_COMMISSION_PERIOD*SECOND);
        found = search(hint);
        S.channel = found ? found : hint;
        letv_leds(BOARD_LED_GREEN);
#if GPD_SECURITY
        send(gpd_commissioning(frame, S.seq, S.src, settings.key, take()), 0);
#else
        send(gpd_commissioning(frame, S.seq, S.src), 0);
#endif
        nap(GPD_REPEAT_GAP, SLEEP_PM2);
        report();
        letv_leds(0);
        if (!found && S.rounds < GPD_COMMISSION_ROUNDS && sleep_until(round, SLEEP_PM2, 1)) break;
    }
    if (found) {
        S.found++;
        settings.channel = found;
        letv_leds(BOARD_LED_GREEN); nap(SECOND, SLEEP_PM2); letv_leds(0);
    } else {
        blink(BOARD_LED_RED, 3);
    }
    settings.profile = SETTINGS_PROFILE;
    settings_save(&settings);
    paired = 1;
    deadline = LATER((uint32_t)GPD_HEARTBEAT_CHAIN*SECOND);
}

static void decommission(void)
{
    if (paired) send(SECURE(gpd_decommissioning(frame, S.seq, S.src)), 0);
    settings_clear();
    settings.channel = settings.profile = 0;
#if GPD_SECURITY
    /* The key goes; the counters stay used. */
    settings.keyed = 0;
    settings_save(&settings);
#endif
    paired = 0;
    blink(BOARD_LED_RED, 5);
}

/* From a wake on a new contact until every contact is open: sample every
 * BUTTON_POLL (the first after the debounce, the CPU idle), a sample equal
 * to the previous one is debounced and its changes are sent (when paired)
 * and fed to the combo. After BUTTON_STUCK without a change, sample every
 * BUTTON_SLOW, waking early for a new contact. The combo's action on the
 * final release. */
static void buttons(void)
{
    uint8_t seen, now, command, slow = 0, action = GPD_COMBO_NONE, mode = SLEEP_IDLE;
    uint32_t changed;
    S.stage = 3;
    gpd_combo_reset(&combo);
    seen = LETV_BUTTONS();
    changed = NOW();
    for (;;) {
        if (slow) {
            held = S.contacts;
            (void)sleep_until(LATER(BUTTON_SLOW), SLEEP_PM2, 1);
            held = 0;
        } else {
            nap(BUTTON_POLL, mode);
            mode = SLEEP_PM2;
        }
        heartbeat();
        now = LETV_BUTTONS();
        if (now != seen) {
            seen = now;
            slow = 0;
            continue;
        }
        action = gpd_combo(&combo, now, NOW());
        while ((command = gpd_step(S.contacts, now)) != 0) {
            S.contacts = gpd_step_contacts(S.contacts, now);
            if (command == GPD_PRESS) S.presses++;
            if (paired)
                send(SECURE(gpd_contact(frame, S.seq, S.src, command, S.contacts)),
                     GPD_PRESS_FLASH && command == GPD_PRESS && !combo.level);
            changed = NOW();
            slow = 0;
        }
        letv_leds(gpd_combo_leds(&combo, NOW()));
        if (!now) break;
        if (!slow && SINCE(changed) >= BUTTON_STUCK) slow = 1;
    }
    letv_leds(0);
    if (action == GPD_COMBO_DECOMMISSION) decommission();
    else if (action == GPD_COMBO_COMMISSION) commission();
}

void main(void)
{
    uint8_t i, fresh = GPD_RESET_COMMISSION
        && (MMIO_READ(SOC_SLEEPSTA) & SLEEPSTA_RST_MASK) < SLEEPSTA_RST_WATCHDOG;
    letv_startup();
    S.stage = 1;
    if (clock_select_init(CLOCK_XOSC32, 1024, 4096, &clock_diagnostics) != CLOCK_OK) restart(1);
    letv_buttons_arm();
    for (i = 0; i < 8u; i++) ieee[i] = MMIO_XREAD(INFO_IEEE + i);
    gpd_source(S.src, ieee);
    S.seq = (uint8_t)(ieee[0] ^ MMIO_READ(SOC_ST0));
    settings_load(&settings);
#if GPD_SECURITY
    counter = (uint32_t)settings.lease << 10;
#endif
    S.channel = settings.channel ? settings.channel : GPD_CHANNEL;
    letv_measure(&values);
    if (!settings.channel || settings.profile != SETTINGS_PROFILE) fresh = 1;
#if GPD_SECURITY
    if (settings.keyed != 0xffu) fresh = 1;
#endif
    if (fresh) {
        commission();
    } else {
        paired = 1;
        report();
        deadline = LATER((uint32_t)GPD_HEARTBEAT_CHAIN*SECOND);
    }
    for (;;) {
        if (sleep_until(deadline, SLEEP_PM2, 1)) buttons();
        else heartbeat();
    }
}
