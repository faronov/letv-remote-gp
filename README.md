# LeTV remote Green Power firmware (CC2530)

[![CI](https://github.com/faronov/letv-remote-gp/actions/workflows/ci.yml/badge.svg)](https://github.com/faronov/letv-remote-gp/actions/workflows/ci.yml)

Firmware for the 8-button LeTV keypad (CC2530F256, CR2032 coin cell) that
turns it into a **Zigbee Green Power Device (GPD)**, a Generic Switch. It
never joins a network. It sleeps in PM2. Any button press wakes it and goes
out at once as a secured GP Press or Release frame carrying all eight
contacts. A Green Power proxy or sink, such as ZHA with the bundled quirk,
receives it.

Derived from [faronov/efektalab-bgt1-gp](https://github.com/faronov/efektalab-bgt1-gp),
which provides the GPD core: frames, security, channel search, flash
settings and the PM2 sleep. The board description comes from
[diyruz/freepad](https://github.com/diyruz/freepad) (`HAL_BOARD_LETV`).

- Image: about 14 KB, non-banked.
- Toolchain: LLVM MCS-51 ([faronov/llvm-project](https://github.com/faronov/llvm-project)),
  pinned through the [cc2530-zigbee](https://github.com/faronov/cc2530-zigbee)
  submodule. Only the stack's clock, Sleep Timer and AES helpers are linked.

## Hardware

| Contact bit | Pin | Button | ZHA entity |
|---|---|---|---|
| 0 (0x01) | P0.0 | 1, "back" | Back |
| 1 (0x02) | P0.1 | 2, ring bottom | Ring bottom |
| 2 (0x04) | P0.2 | 3, ring right | Ring right |
| 3 (0x08) | P0.3 | 4, ring top | Ring top |
| 4 (0x10) | P0.4 | 5, ring left | Ring left |
| 5 (0x20) | P0.5 | 6, "mic" | Mic |
| 6 (0x40) | P0.6 | 7, "play/pause" | Play/Pause |
| 7 (0x80) | P0.7 | 8, OK/center | Center |

| Function | Pin |
|---|---|
| Buttons (active low, internal pull-ups, falling-edge port 0 interrupt) | P0.0–P0.7 |
| Red LED (active low) | P1.0 |
| Green LED (active low) | P1.1 |
| Battery | VDD/3 through the internal ADC channel (no pin) |

Each button pulls its pin to ground, one pin per button. The firmware
enables the port 0 internal pull-ups (`P0INP` 0, `P2INP.PDUP0` 0) and does
not rely on external ones. The other pins keep their reset state: inputs
with pull-ups. P2.3/P2.4 hold the 32-kHz crystal.

## Buttons

- **Any change** of the contacts sends one frame immediately (see
  Protocol). The firmware first debounces for 20 ms, then samples every
  20 ms while any button is held. Two equal samples in a row count as a
  stable state.
- **Several buttons** at once work too. Every frame carries the full bitmap
  of the buttons closed now.
- **Mic + OK/center** (buttons 6 and 8, mask 0xA0, nothing else held). The
  action runs when both are released:

  | Held | LEDs while held | On release |
  |---|---|---|
  | < 3 s | — | nothing (normal Press/Release only) |
  | 3–10 s | green blinks (250 ms) | channel search and commissioning |
  | ≥ 10 s | solid red | Decommissioning (0xE1), clear settings and key, 5 red blinks |

  The combo's Press/Release frames are still sent. The combo is cancelled
  in any of these cases:
  - another button is pressed during the session;
  - the pair is pressed again after one of the two was released;
  - the pair is held for 30 s, which is treated as a stuck key.
- **Stuck contacts.** After 30 s without a change the firmware samples
  only once a second; a new button still wakes it at once.
- **First start.** With no saved channel, settings from another firmware,
  or no key, the device commissions by itself.

## LEDs

| Signal | Meaning |
|---|---|
| green ~20 ms flash | a Press frame was sent (`GPD_DEFINES=-DGPD_PRESS_FLASH=0` disables it) |
| green blinking while Mic + OK is held | 3 s reached: commission on release |
| solid red while Mic + OK is held | 10 s reached: decommission on release |
| green flash per channel-search attempt | commissioning in progress |
| green 1 s | the channel was found |
| 3 red blinks | no channel answer; the hint channel is used |
| 5 red blinks | decommissioned |

Otherwise both LEDs are off.

## Protocol

- Frame type: Green Power Device Frames, ApplicationID 0 (SrcID),
  unidirectional, security level 3 (see Security).
- SrcID: the low 32 bits of the chip IEEE address, avoiding the reserved
  values.
- Commissioning frame (0xE0):

  | Field | Value |
  |---|---|
  | DeviceID | 0x07 (Generic Switch) |
  | Options | 0x85: MAC sequence number capability, Extended Options present, Application Information present |
  | Extended Options | 0xF3: level 3, individual key, key present, encrypted, counter present |
  | Application Information | 0x1B: manufacturer ID, model ID, cluster list and switch information present |
  | Manufacturer | 0x6D6B |
  | Model | 0xF500 |
  | Clusters | one server cluster, 0x0001 Power Configuration; no client clusters |
  | Switch Information | `02 18 00`: length 2, 8 contacts, switch type 1 (button), contact status 0 |

- Buttons:
  - 8-bit vector **Press (0x69)** when contacts were added;
  - 8-bit vector **Release (0x6A)** when contacts were removed.

  The payload is the bitmap of the contacts closed now (bit n = button n+1);
  a Release of the last button carries 0x00. A press and a release in the
  same sample go out as two frames, the Release first.
- Battery: Attribute Reporting (0xA0) of cluster 0x0001 with
  BatteryVoltage (0x0020, 100 mV) and BatteryPercentageRemaining (0x0021,
  0.5 %), both uint8 (0x20). It is sent after each commissioning, at every
  start, and every 6 h. The 6-h heartbeat is a chain of 240-s Sleep Timer
  wakes, because the 24-bit timer wraps at 512 s; every 90th wake reports.
  The values come from VDD measured at the end of the transmissions of the
  frame sent before (under TX load). The percentage is linear between the
  points of an assumed CR2032 curve: 100 % at 3.0 V, 80 % at 2.9 V, 60 % at
  2.8 V, 40 % at 2.7 V, 20 % at 2.6 V, 5 % at 2.4 V and 0 % at 2.0 V.
- Repeats: each GPDF goes out 3 times with the same sequence number and
  frame counter, 20 ms apart, PM2 in between. The last copy ends about
  45 ms after the first started; the sink drops the duplicates.
- TX power: `GPD_TXPOWER` 0xF5 (+4.5 dBm).

### Channel search and commissioning

Commissioning runs up to 12 rounds, 10 s apart. Each round works as
follows:

1. **Channel search (GP Basic A.3.9.1).** Up to 6 attempts, 400 ms apart.
   Each attempt sends a Maintenance *Channel Request* (0xE3) on every
   channel, with Auto-Commissioning set. The *hint* channel (the saved one,
   or `GPD_CHANNEL` = 15) goes last, with Auto-Commissioning cleared. The
   device then listens there for 18–40 ms.
2. **Answer.** A GP proxy of the network (the TempMaster) can answer with a
   *Channel Configuration* (0xF3) carrying the operational channel. It does
   so only while the sink is in commissioning mode, which in ZHA means
   permit join is open.
3. **Commissioning.** An answer ends the rounds: one Commissioning frame
   and a battery report go out on that channel, and the channel is saved.
   Without an answer, the round sends them on the hint channel instead.

A button press between rounds ends the rounds.

The ZHA coordinator itself cannot answer: bellows/EZSP never sends GP
frames to a GPD. The answer therefore needs a GP proxy, for example an IKEA
TRÅDFRI router, within radio range. Without one, keep the network on
channel 15 or rebuild with another hint:
`make firmware GPD_DEFINES=-DGPD_CHANNEL=20`.

### Security

Press, Release, battery and Decommissioning frames are sent at GP security
level 3 (encrypted, 4-byte MIC, AES-CCM* on the CC2530 AES coprocessor) with
an individual out-of-the-box key (KeyType 4). Channel Requests are
unprotected, as the specification requires.

- Key: 16 random bytes, made on the first commissioning from the radio's
  random bits and whitened with AES. It survives recommissioning and goes on
  decommissioning or a chip erase. The sink then sees a new key and the
  device must be paired again.
- Commissioning frame: the key is sent encrypted with the ZigBee Alliance
  default TC link key (`ZigBeeAlliance09`) with its MIC, plus the outgoing
  frame counter, as ZHA (zigpy) and Zigbee2MQTT (zigbee-herdsman) expect.
- Frame counter: 32 bits, never reused for a given key. Flash holds a lease
  of 1024 counter values; each reboot starts at the next lease.
- After decommissioning the device sends no button frames until it is
  commissioned again.
- `GPD_DEFINES=-DGPD_SECURITY=0` builds the unsecured variant.

### Settings

The channel, the firmware profile, the key and the frame-counter lease are
kept in flash page 125 (0x3E800), as a log of 4-byte records written by DMA.
A chip erase (`cc-tool -e`) therefore means a first start and a new key.

## Pairing with ZHA

1. Copy `quirks/zha_quirk_letv_remote_gp.py` ("LeTV GP remote") to
   `<HA config>/custom_zha_quirks/` and restart Home Assistant. This needs a
   ZHA build with Green Power support.
2. Permit joining.
3. Insert the battery (first start), or hold Mic + OK for 3–10 s and release.

The quirk matches DeviceID 0x07, manufacturer 0x6D6B and model 0xF500. It
adds:

- eight button event entities (Back, Ring bottom, Ring right, Ring top,
  Ring left, Mic, Play/Pause, Center). Their event types are:
  - `press`;
  - `release`;
  - `short_press` (released less than 1 s after the press);
  - `long_press` (held 1 s or more, fired on release).

  Release events carry `duration` in seconds.
- the Battery (%) and Battery voltage (V) sensors, from Attribute Reporting
  or Multi-Cluster Reporting frames.

The per-button state follows the bitmap of each frame, so repeated masks
fire nothing. There is no `hold` event while a button is still down. The
Mic + OK gesture also fires its normal events.

## Flashing

The stock firmware is debug-locked, so the original image cannot be read
back. Flashing with an erase replaces it for good. Use a CC Debugger:

```sh
cc-tool -e -w build/firmware/letv-remote-gp.hex -v
```

Release builds can be downloaded from the
[latest release](https://github.com/faronov/letv-remote-gp/releases/latest).

## Building and testing

```sh
git clone --recursive https://github.com/faronov/letv-remote-gp
cd letv-remote-gp
make test                       # frame builder, button logic (host, ASan/UBSan)
make test-zigpy                 # needs: pip install pytest zigpy==2.4.1
make test-quirk                 # needs: pip install pytest zha zha-quirks
make firmware LLVM_TOOLS=<llvm-build>/bin LLVM_SRC=<llvm-project>
```

- `make test` checks the following:
  - the commissioning, press, release and battery frames byte by byte;
  - the security vectors;
  - the press/release decision for all 65536 mask pairs;
  - the Mic + OK timing classification.
- `make test-zigpy` feeds the host test's frames through zigpy's GPDF
  parser, commissioning-key decryption and command schemas. It covers
  8 contacts, button switch type, cluster 0x0001 and the contact bitmaps.
  `ZIGPY_SRC=<zigpy checkout>` uses a source tree instead of the installed
  package.

`LLVM_SRC` must be the [faronov/llvm-project](https://github.com/faronov/llvm-project)
checkout at the revision in `cc2530-zigbee/tools/llvm/toolchain.json`. The
MCS-51 runtime and startup come from it, and `LLVM_TOOLS` must be built from
the same revision (`clang`, `lld`, `llvm-ar`, `llvm-objcopy`; see
`.github/workflows/ci.yml`). The output is
`build/firmware/letv-remote-gp.{hex,elf,map}`.

The image is linked with `cc2530-root.ld`. It is LLVM's `cc2530.ld` without
the code bank sections, so every function stays in the 32-KB common area.

The image has no DATA/IDATA globals, so the build patches the startup to
`mov sp, #0x47`. The hardware stack then gets IDATA 0x48–0xFF (184 bytes)
instead of 0x80–0xFF; the deepest path needs about 160 bytes. After linking,
`tools/stack.py` estimates the worst case from the disassembly and fails the
build if it does not fit. `tools/size.py` checks that the image fits the
32-KB common area and that the reset, Sleep Timer (0x2B) and port 0
(0x6B) interrupt vectors are programmed.

## Power notes

- **Idle.** PM2, the 32-kHz crystal and Sleep Timer running; the radio is
  off, both LEDs are off, and the CPU wakes only on a button edge or a
  heartbeat-chain wake (240 s). The datasheet gives about 1 µA for PM2. With
  all buttons open the pull-ups draw nothing.
- **A held button** draws its pull-up current, about 3 V across roughly
  20 kΩ (≈150 µA). While buttons are held, the firmware wakes every 20 ms,
  or every 1 s after 30 s without a change.
- **Each button change** costs:
  - one 20-ms debounce, with the CPU idle on the 32-MHz crystal;
  - three short transmissions of about 1.4 ms each, at about 34 mA;
  - with `GPD_PRESS_FLASH`, a 20-ms LED flash.
- **Commissioning** keeps the radio busy for a while: 16 Channel Requests
  and an 18–40 ms receive window per attempt, up to 6 attempts per round
  and 12 rounds. It only runs on the first start or on request.

## Releases

Pushing a `v*` tag builds the firmware in CI. The workflow then publishes
`letv-remote-gp.hex`, `letv-remote-gp.elf`, `SHA256SUMS` and the ZHA quirk
as a GitHub release.

## License

BSD-3-Clause, see [LICENSE](LICENSE). `cc2530-root.ld` is derived from LLVM
(Apache-2.0 WITH LLVM-exception).
