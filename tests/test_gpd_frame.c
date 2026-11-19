/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, cc2530-zigbee contributors. See LICENSE.
 */
/* The Green Power frames of the LeTV remote GP firmware
 * (include/gpd_frame.h) against hand-assembled bytes (Zigbee Green Power
 * Basic: MAC header, NWK frame control 0x0C, SrcID, command, payload), and
 * the button logic (include/gpd_buttons.h). With an argument, prints the
 * frames as hex for an external parser cross-check
 * (tests/test_zigpy_frames.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MCU_XDATA
#include "gpd_frame.h"
#include "gpd_buttons.h"
#if GPD_SECURITY
#include "aes_reference.h"

void gpd_aes(const uint8_t *key, uint8_t *block)
{
    aes_reference_encrypt(key, block, block);
}

/* zigpy.zgp.crypto (AESCCM of the Python cryptography package) for SrcID
 * 0x0EF62815 and the key C0..CF: the key protected with the TC-LK and its
 * MIC; SecurityLevel 3 frames: the BGT1 GP report (bgt1_report below) with
 * security frame counter 0x401, the Decommissioning GPDF with 0x12345678,
 * Press A0 with 0x402, Release 80 with 0x403 and the battery report with
 * 0x404. */
static const uint8_t gpd_key[16] = {
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf
};
static const uint8_t protected_key[20] = {
    0x47, 0x4e, 0x69, 0xa9, 0xd4, 0xde, 0x13, 0x69, 0xfc, 0x7f, 0xcd, 0x93, 0xb0, 0xa1, 0xd9, 0xfa,
    0xf7, 0x12, 0xd5, 0x07
};
static const uint8_t secured_bgt1_report[] = {
    0x01, 0x08, 0x43, 0xff, 0xff, 0xff, 0xff, 0x8c, 0x38, 0x15, 0x28, 0xf6, 0x0e, 0x01, 0x04, 0x00, 0x00,
    0xcd, 0x48, 0xa1, 0x02, 0xe5, 0x0e, 0x9d, 0x0c, 0x28, 0xac, 0x61, 0xbe, 0x9c, 0xf7, 0x32, 0x21,
    0x7f, 0x12, 0x2e, 0xdc, 0x08, 0x63, 0x11, 0xd4
};
static const uint8_t secured_decommissioning[] = {
    0x01, 0x08, 0x47, 0xff, 0xff, 0xff, 0xff, 0x8c, 0x38, 0x15, 0x28, 0xf6, 0x0e, 0x78, 0x56, 0x34, 0x12,
    0x9a, 0x7e, 0xc8, 0xf1, 0x57
};
static const uint8_t secured_press[] = {
    0x01, 0x08, 0x50, 0xff, 0xff, 0xff, 0xff, 0x8c, 0x38, 0x15, 0x28, 0xf6, 0x0e, 0x02, 0x04, 0x00, 0x00,
    0x5f, 0xf5, 0x30, 0x28, 0x6e, 0xdd
};
static const uint8_t secured_release[] = {
    0x01, 0x08, 0x51, 0xff, 0xff, 0xff, 0xff, 0x8c, 0x38, 0x15, 0x28, 0xf6, 0x0e, 0x03, 0x04, 0x00, 0x00,
    0xa1, 0x82, 0x00, 0xa0, 0x6e, 0x5f
};
static const uint8_t secured_battery[] = {
    0x01, 0x08, 0x52, 0xff, 0xff, 0xff, 0xff, 0x8c, 0x38, 0x15, 0x28, 0xf6, 0x0e, 0x04, 0x04, 0x00, 0x00,
    0x6e, 0xf1, 0xb2, 0x86, 0x91, 0xf1, 0xdf, 0x40, 0xe4, 0x4a, 0x09, 0x07, 0x81, 0x5a, 0x6a
};
#define COMMISSIONING(f, seq, src) gpd_commissioning(f, seq, src, gpd_key, 0x400u)
#else
#define COMMISSIONING(f, seq, src) gpd_commissioning(f, seq, src)
#endif

#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "gpd_frame: line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned long checks;
static uint8_t expected[GPD_COMMISSIONING_LENGTH];

/* A NoSecurity Commissioning GPDF of n bytes as built with GPD_SECURITY:
 * options 85, extended options F3, the protected key and its MIC, the
 * outgoing counter 0x400, then the application information onwards. */
static const uint8_t *commissioned(const uint8_t *plain, uint8_t n)
{
#if GPD_SECURITY
    static const uint8_t counter[4] = {0x00, 0x04, 0x00, 0x00};
    memcpy(expected, plain, 14);
    expected[14] = 0x85; expected[15] = 0xf3;
    memcpy(expected + 16, protected_key, 20);
    memcpy(expected + 36, counter, 4);
    memcpy(expected + 40, plain + 15, n - 15u);
#else
    memcpy(expected, plain, n);
#endif
    return expected;
}

static void hex(const char *name, const uint8_t *f, uint8_t n)
{
    uint8_t i;
    printf("%s ", name);
    for (i = 0; i < n; i++) printf("%02x", f[i]);
    printf("\n");
}

/* The frames gpd_step sends from sent toward now, as command and payload
 * pairs into out; their count. */
static unsigned steps(uint8_t sent, uint8_t now, uint8_t out[][2])
{
    unsigned n = 0;
    uint8_t command;
    while ((command = gpd_step(sent, now)) != 0) {
        sent = gpd_step_contacts(sent, now);
        out[n][0] = command; out[n][1] = sent;
        if (++n > 4u) break;
    }
    return n;
}

#define S(t) ((uint32_t)((t) * GPD_TICKS_SECOND))

static void test_buttons(void)
{
    uint8_t out[5][2];
    gpd_combo_t c;
    unsigned mask, sent;

    /* Press, more pressed, partial and full release, a swap. */
    CHECK(steps(0x00, 0x01, out) == 1 && out[0][0] == GPD_PRESS && out[0][1] == 0x01);
    CHECK(steps(0x01, 0x81, out) == 1 && out[0][0] == GPD_PRESS && out[0][1] == 0x81);
    CHECK(steps(0x81, 0x80, out) == 1 && out[0][0] == GPD_RELEASE && out[0][1] == 0x80);
    CHECK(steps(0x80, 0x00, out) == 1 && out[0][0] == GPD_RELEASE && out[0][1] == 0x00);
    CHECK(steps(0x01, 0x02, out) == 2 && out[0][0] == GPD_RELEASE && out[0][1] == 0x00 &&
          out[1][0] == GPD_PRESS && out[1][1] == 0x02);
    CHECK(steps(0x03, 0x06, out) == 2 && out[0][0] == GPD_RELEASE && out[0][1] == 0x02 &&
          out[1][0] == GPD_PRESS && out[1][1] == 0x06);
    /* A duplicate mask sends nothing. */
    CHECK(steps(0x00, 0x00, out) == 0 && steps(0xa0, 0xa0, out) == 0);
    /* Exhaustive: at most two frames, ending at now, the payload always
     * the contacts closed (a Release never adds, a Press never removes). */
    for (sent = 0; sent < 256u; sent++)
        for (mask = 0; mask < 256u; mask++) {
            unsigned n = steps((uint8_t)sent, (uint8_t)mask, out);
            CHECK(n <= 2u && (n == 0u) == (sent == mask));
            if (n) CHECK(out[n-1u][1] == mask);
            if (n == 2u) CHECK(out[0][0] == GPD_RELEASE && out[1][0] == GPD_PRESS);
            if (n && out[0][0] == GPD_RELEASE) CHECK((out[0][1] & ~sent) == 0);
            if (n && out[0][0] == GPD_PRESS) CHECK(n == 1u && (sent & ~mask) == 0);
        }

    /* Combo Mic (P0.5) + OK (P0.7). */
    CHECK(GPD_COMBO_MASK == 0xa0u);
    /* Released under 3 s: nothing. */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0xa0, S(1)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(3.9)) == GPD_COMBO_NONE && c.level == GPD_COMBO_NONE);
    CHECK(gpd_combo_leds(&c, S(3.9)) == 0);
    CHECK(gpd_combo(&c, 0x00, S(4)) == GPD_COMBO_NONE);
    /* 3 s: green blinking, commission on release, the first release of
     * the pair keeps it. */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0x80, S(0)) == GPD_COMBO_NONE && c.state == GPD_COMBO_IDLE);
    CHECK(gpd_combo(&c, 0xa0, S(0.1)) == GPD_COMBO_NONE && c.state == GPD_COMBO_RUNNING);
    CHECK(gpd_combo(&c, 0xa0, S(3.05)) == GPD_COMBO_NONE && c.level == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(3.1)) == GPD_COMBO_NONE && c.level == GPD_COMBO_COMMISSION);
    CHECK(gpd_combo_leds(&c, S(3.1)) == GPD_LED_GREEN);
    CHECK(gpd_combo_leds(&c, S(3.1 + 0.3)) == 0);
    CHECK(gpd_combo_leds(&c, S(3.1 + 0.55)) == GPD_LED_GREEN);
    CHECK(gpd_combo(&c, 0x20, S(9.9)) == GPD_COMBO_NONE && c.state == GPD_COMBO_ARMED);
    CHECK(gpd_combo(&c, 0x20, S(12)) == GPD_COMBO_NONE && c.level == GPD_COMBO_COMMISSION);
    CHECK(gpd_combo(&c, 0x00, S(12.1)) == GPD_COMBO_COMMISSION && c.state == GPD_COMBO_IDLE);
    /* 10 s: solid red, decommission on release. */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0xa0, S(100)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(110)) == GPD_COMBO_NONE && c.level == GPD_COMBO_DECOMMISSION);
    CHECK(gpd_combo_leds(&c, S(110)) == GPD_LED_RED && gpd_combo_leds(&c, S(110.3)) == GPD_LED_RED);
    CHECK(gpd_combo(&c, 0x00, S(111)) == GPD_COMBO_DECOMMISSION);
    /* Across the 24-bit Sleep Timer wrap (512 s). */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0xa0, S(510)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(521) & GPD_TICKS_MASK) == GPD_COMBO_NONE && c.level == GPD_COMBO_DECOMMISSION);
    CHECK(gpd_combo(&c, 0x00, S(522) & GPD_TICKS_MASK) == GPD_COMBO_DECOMMISSION);
    /* Another button at any time cancels until all are released. */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0xa0, S(0)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(5)) == GPD_COMBO_NONE && c.level == GPD_COMBO_COMMISSION);
    CHECK(gpd_combo(&c, 0xa1, S(6)) == GPD_COMBO_NONE && c.state == GPD_COMBO_CANCELLED);
    CHECK(c.level == GPD_COMBO_NONE && gpd_combo_leds(&c, S(6)) == 0);
    CHECK(gpd_combo(&c, 0xa0, S(20)) == GPD_COMBO_NONE && c.level == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0x00, S(21)) == GPD_COMBO_NONE);
    /* A button before the combo: no combo in that session. */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0x41, S(0)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xe1, S(0.1)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(0.2)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(11)) == GPD_COMBO_NONE && c.level == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0x00, S(12)) == GPD_COMBO_NONE);
    /* The old combo (buttons 1 and 7) does nothing. */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0x41, S(0)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0x41, S(11)) == GPD_COMBO_NONE && c.level == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0x00, S(12)) == GPD_COMBO_NONE);
    /* Pressing the pair again after a partial release cancels. */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0xa0, S(0)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(4)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0x80, S(4.5)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(5)) == GPD_COMBO_NONE && c.state == GPD_COMBO_CANCELLED);
    CHECK(gpd_combo(&c, 0x00, S(5.5)) == GPD_COMBO_NONE);
    /* Held 30 s: stuck, cancelled, LEDs off. */
    gpd_combo_reset(&c);
    CHECK(gpd_combo(&c, 0xa0, S(0)) == GPD_COMBO_NONE);
    CHECK(gpd_combo(&c, 0xa0, S(29.9)) == GPD_COMBO_NONE && c.level == GPD_COMBO_DECOMMISSION);
    CHECK(gpd_combo(&c, 0xa0, S(30)) == GPD_COMBO_NONE && c.state == GPD_COMBO_CANCELLED);
    CHECK(gpd_combo_leds(&c, S(30)) == 0);
    CHECK(gpd_combo(&c, 0x00, S(31)) == GPD_COMBO_NONE);
    /* After a session the next one starts clean. */
    CHECK(gpd_combo(&c, 0xa0, S(40)) == GPD_COMBO_NONE && c.state == GPD_COMBO_RUNNING);
    CHECK(gpd_combo(&c, 0xa0, S(43)) == GPD_COMBO_NONE && gpd_combo(&c, 0, S(43.5)) == GPD_COMBO_COMMISSION);
}

int main(int argc, char **argv)
{
    static const uint8_t ieee[8] = {0x15, 0x28, 0xf6, 0x0e, 0x00, 0x4b, 0x12, 0x00};
    /* GenericSwitch 0x07, options 05 (MAC sequence number, application
     * information), application information 1B (manufacturer, model,
     * cluster list, switch information), 6D6B, model F500, one server
     * cluster (Power Configuration), switch information: length 2, 8
     * contacts of a button switch (0x18), all open. */
    static const uint8_t commissioning[] = {
        0x01, 0x08, 0x42, 0xff, 0xff, 0xff, 0xff, 0x0c, 0x15, 0x28, 0xf6, 0x0e, 0xe0,
        0x07, 0x05, 0x1b, 0x6b, 0x6d, 0x00, 0xf5,
        0x01, 0x01, 0x00,
        0x02, 0x18, 0x00
    };
    static const uint8_t press[] = {
        0x01, 0x08, 0x50, 0xff, 0xff, 0xff, 0xff, 0x0c, 0x15, 0x28, 0xf6, 0x0e, 0x69, 0xa0
    };
    static const uint8_t release[] = {
        0x01, 0x08, 0x51, 0xff, 0xff, 0xff, 0xff, 0x0c, 0x15, 0x28, 0xf6, 0x0e, 0x6a, 0x80
    };
    static const uint8_t release_all[] = {
        0x01, 0x08, 0x53, 0xff, 0xff, 0xff, 0xff, 0x0c, 0x15, 0x28, 0xf6, 0x0e, 0x6a, 0x00
    };
    /* Battery 3.0 V (30 * 100 mV) and 90 % (180 * 0.5 %). */
    static const uint8_t battery[] = {
        0x01, 0x08, 0x52, 0xff, 0xff, 0xff, 0xff, 0x0c, 0x15, 0x28, 0xf6, 0x0e, 0xa0,
        0x01, 0x00, 0x20, 0x00, 0x20, 0x1e, 0x21, 0x00, 0x20, 0xb4
    };
#if GPD_SECURITY
    /* The BGT1 GP report (Multi-Cluster Reporting), to keep its zigpy MIC
     * vector as a check of gpd_secure. */
    static const uint8_t bgt1_report[] = {
        0x01, 0x08, 0x43, 0xff, 0xff, 0xff, 0xff, 0x0c, 0x15, 0x28, 0xf6, 0x0e, 0xa2,
        0x02, 0x04, 0x00, 0x00, 0x29, 0x0c, 0xfe,
        0x01, 0x00, 0x20, 0x00, 0x20, 0x1e,
        0x01, 0x00, 0x21, 0x00, 0x20, 0xc8
    };
#endif
    /* Maintenance Channel Requests announcing channel 15 (0b0100 twice). */
    static const uint8_t request_more[] = {0x01, 0x08, 0x45, 0xff, 0xff, 0xff, 0xff, 0x4d, 0xe3, 0x44};
    static const uint8_t request_last[] = {0x01, 0x08, 0x46, 0xff, 0xff, 0xff, 0xff, 0x0d, 0xe3, 0xff};
    static const uint8_t decommissioning[] = {
        0x01, 0x08, 0x47, 0xff, 0xff, 0xff, 0xff, 0x0c, 0x15, 0x28, 0xf6, 0x0e, 0xe1
    };
    /* Channel Configurations (two FCS status bytes): from a proxy with a
     * short source address and PAN ID compression, channel 20 with the
     * Basic bit; without a source address, channel 11. */
    static const uint8_t config_proxy[] = {
        0x41, 0x88, 0x10, 0x62, 0x1a, 0xff, 0xff, 0x34, 0x12, 0x0d, 0xf3, 0x19, 0xd0, 0x80
    };
    static const uint8_t config_bare[] = {0x01, 0x08, 0x11, 0xff, 0xff, 0xff, 0xff, 0x0d, 0xf3, 0x00, 0x00, 0x80};
    uint8_t src[4], f[GPD_FRAME_LENGTH + 16u], m[sizeof(config_proxy)], n, z[4] = {0}, high[4] = {0xfe, 0xff, 0xff, 0xff};
    gpd_values_t v = { 30, 180 };
    (void)argv;

    gpd_source(src, ieee);
    CHECK(src[0] == 0x15 && src[1] == 0x28 && src[2] == 0xf6 && src[3] == 0x0e);
    /* Reserved SrcIDs: 0 and FFFFFFF9..FFFFFFFF. */
    gpd_source(src, z);
    CHECK(src[0] == 1 && !src[1] && !src[2] && !src[3]);
    gpd_source(src, high);
    CHECK(src[0] == 0xf6);
    high[0] = 0xff; gpd_source(src, high); CHECK(src[0] == 0xf7);
    high[0] = 0xf9; gpd_source(src, high); CHECK(src[0] == 0xf1);
    high[0] = 0xf8; gpd_source(src, high); CHECK(src[0] == 0xf8);
    high[3] = 0xfe; high[0] = 0xff; gpd_source(src, high); CHECK(src[0] == 0xff);

    gpd_source(src, ieee);
    CHECK(GPD_SWITCH_CONFIGURATION == 0x18u && GPD_CONTACTS == 8u);
    memset(f, 0xaa, sizeof(f));
    n = COMMISSIONING(f, 0x42, src);
#if GPD_SECURITY
    CHECK(n == 51u);
#else
    CHECK(n == 26u);
#endif
    CHECK(n == GPD_COMMISSIONING_LENGTH);
    CHECK(!memcmp(f, commissioned(commissioning, sizeof(commissioning)), n) && f[n] == 0xaa);
    if (argc > 1) hex("commissioning", f, n);

    memset(f, 0xaa, sizeof(f));
    n = gpd_contact(f, 0x50, src, GPD_PRESS, 0xa0);
    CHECK(n == GPD_CONTACT_LENGTH && n == sizeof(press) && !memcmp(f, press, n) && f[n] == 0xaa);
    if (argc > 1) hex("press", f, n);
    n = gpd_contact(f, 0x51, src, GPD_RELEASE, 0x80);
    CHECK(n == sizeof(release) && !memcmp(f, release, n));
    if (argc > 1) hex("release", f, n);
    n = gpd_contact(f, 0x53, src, GPD_RELEASE, 0x00);
    CHECK(n == sizeof(release_all) && !memcmp(f, release_all, n));
    if (argc > 1) hex("release-all", f, n);
    memset(f, 0xaa, sizeof(f));
    n = gpd_battery(f, 0x52, src, &v);
    CHECK(n == GPD_BATTERY_LENGTH && n == sizeof(battery) && !memcmp(f, battery, n) && f[n] == 0xaa);
    if (argc > 1) hex("battery", f, n);

    /* PSDU with the FCS within aMaxPHYPacketSize. */
    CHECK(GPD_FRAME_LENGTH + 2u <= 127u);
    CHECK(GPD_COMMISSIONING_LENGTH <= GPD_FRAME_LENGTH);
    CHECK(GPD_BATTERY_LENGTH + GPD_SECURITY_OVERHEAD <= GPD_FRAME_LENGTH);
    CHECK(GPD_CONTACT_LENGTH + GPD_SECURITY_OVERHEAD <= GPD_FRAME_LENGTH);
#if GPD_SECURITY
    /* gpd_secure against the BGT1 GP vectors (unchanged code). */
    memset(f, 0xaa, sizeof(f));
    memcpy(f, bgt1_report, sizeof(bgt1_report));
    n = gpd_secure(f, sizeof(bgt1_report), gpd_key, 0x401u);
    CHECK(n == sizeof(secured_bgt1_report) && !memcmp(f, secured_bgt1_report, n) && f[n] == 0xaa);
    memset(f, 0xaa, sizeof(f));
    n = gpd_secure(f, gpd_decommissioning(f, 0x47, src), gpd_key, 0x12345678u);
    CHECK(n == GPD_DECOMMISSIONING_SECURED && n == sizeof(secured_decommissioning));
    CHECK(!memcmp(f, secured_decommissioning, n) && f[n] == 0xaa);
    if (argc > 1) hex("secured-decommissioning", f, n);
    /* The new frames secured. */
    memset(f, 0xaa, sizeof(f));
    n = gpd_secure(f, gpd_contact(f, 0x50, src, GPD_PRESS, 0xa0), gpd_key, 0x402u);
    CHECK(n == GPD_CONTACT_LENGTH + GPD_SECURITY_OVERHEAD && n == sizeof(secured_press));
    CHECK(!memcmp(f, secured_press, n) && f[n] == 0xaa);
    if (argc > 1) hex("secured-press", f, n);
    n = gpd_secure(f, gpd_contact(f, 0x51, src, GPD_RELEASE, 0x80), gpd_key, 0x403u);
    CHECK(n == sizeof(secured_release) && !memcmp(f, secured_release, n));
    if (argc > 1) hex("secured-release", f, n);
    memset(f, 0xaa, sizeof(f));
    n = gpd_secure(f, gpd_battery(f, 0x52, src, &v), gpd_key, 0x404u);
    CHECK(n == GPD_BATTERY_LENGTH + GPD_SECURITY_OVERHEAD && n == sizeof(secured_battery));
    CHECK(!memcmp(f, secured_battery, n) && f[n] == 0xaa);
    if (argc > 1) hex("secured-battery", f, n);
    /* A tampered key changes the MIC. */
    memset(f, 0xaa, sizeof(f));
    memcpy(expected, gpd_key, 16); expected[15] ^= 1;
    n = gpd_secure(f, gpd_decommissioning(f, 0x47, src), expected, 0x12345678u);
    CHECK(memcmp(f + n - 4u, secured_decommissioning + n - 4u, 4));
#endif

    CHECK(gpd_channel_request(f, 0x45, 1, 15) == sizeof(request_more));
    CHECK(!memcmp(f, request_more, sizeof(request_more)));
    if (argc > 1) hex("channel-request", f, sizeof(request_more));
    CHECK(gpd_channel_request(f, 0x46, 0, 26) == sizeof(request_last));
    CHECK(!memcmp(f, request_last, sizeof(request_last)));
    CHECK(gpd_decommissioning(f, 0x47, src) == sizeof(decommissioning));
    CHECK(!memcmp(f, decommissioning, sizeof(decommissioning)));

    memcpy(m, config_proxy, sizeof(m));
    CHECK(gpd_channel_configuration(m, sizeof(config_proxy)) == 20u);
    memcpy(m, config_bare, sizeof(config_bare));
    CHECK(gpd_channel_configuration(m, sizeof(config_bare)) == 11u);
    CHECK(!gpd_channel_configuration(m, sizeof(config_bare) - 1u));
    m[7] = GPD_MAINTENANCE_MORE;
    CHECK(!gpd_channel_configuration(m, sizeof(config_bare)));
    m[7] = GPD_MAINTENANCE; m[8] = GPD_CHANNEL_REQUEST;
    CHECK(!gpd_channel_configuration(m, sizeof(config_bare)));
    m[8] = GPD_CHANNEL_CONFIGURATION; m[0] = 0x09;   /* secured */
    CHECK(!gpd_channel_configuration(m, sizeof(config_bare)));
    m[0] = 0x03;                                     /* MAC command */
    CHECK(!gpd_channel_configuration(m, sizeof(config_bare)));
    memcpy(m, config_proxy, sizeof(m));
    m[1] = 0xc8;                                     /* long source: truncated */
    CHECK(!gpd_channel_configuration(m, sizeof(config_proxy)));

    test_buttons();

    if (argc <= 1) printf("gpd_frame: %lu checks passed\n", checks);
    return 0;
}
