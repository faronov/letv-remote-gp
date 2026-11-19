"""Test the letv-remote-gp ZHA quirk against frames produced by include/gpd_frame.h."""

import asyncio
from unittest.mock import MagicMock

import pytest

from zha.application.platforms.event import EntityEventTriggeredEvent
from zha.application.platforms.event.const import ButtonEventType
from zha.const import STATE_CHANGED
from zha.quirks import DEVICE_REGISTRY
from zigpy.device import GreenPowerCommandReceived, GreenPowerDevice
from zigpy.zgp.commands import (
    GPAttributeReportingPayload,
    GPCommissioningPayload,
    GPContactStatusPayload,
    GPMultiClusterReportingPayload,
)
from zigpy.zgp.types import ApplicationID, DeviceID, GPDCommandID, SrcID

import zha_quirk_letv_remote_gp as quirk

# tests/test_gpd_frame.c commissioning payload after the 0xE0 command byte:
# device 0x07, options 0x85, extended options 0xF3, protected key, its MIC,
# outgoing counter 0x400, application info 0x1B, manufacturer 0x6D6B,
# model 0xF500, cluster list (server 0x0001), switch information 02 18 00.
COMMISSIONING = bytes.fromhex(
    "0785f3474e69a9d4de1369fc7fcd93b0a1d9faf712d507000400001b6b6d00f50101000218" "00"
)
# Attribute Reporting (0xA0): cluster 0x0001, 0x0020 = 30 (3.0 V), 0x0021 = 180 (90 %).
ATTRIBUTE_REPORT = bytes.fromhex("0100" "2000201e" "210020b4")
# Multi-Cluster Reporting (0xA2) with the same values.
MULTI_REPORT = bytes.fromhex("0100200020" "1e" "01002100" "20" "c8")
NAMES = [suffix for suffix, _ in quirk.BUTTONS]


class Timer:
    def __init__(self, when: float, callback) -> None:
        self.when, self.callback, self.cancelled = when, callback, False

    def cancel(self) -> None:
        self.cancelled = True


class Clock:
    """Monotonic clock and timer scheduler for the gestures."""

    def __init__(self) -> None:
        self.now = 1000.0
        self.timers: list[Timer] = []

    def __call__(self) -> float:
        return self.now

    def call_later(self, delay: float, callback) -> Timer:
        timer = Timer(self.now + delay, callback)
        self.timers.append(timer)
        return timer

    def pending(self) -> list[Timer]:
        return [t for t in self.timers if not t.cancelled]

    def advance(self, seconds: float) -> None:
        """Move the clock, running the timers due on the way."""
        end = self.now + seconds
        while due := [t for t in self.pending() if t.when <= end]:
            timer = min(due, key=lambda t: t.when)
            self.timers.remove(timer)
            self.now = timer.when
            timer.callback()
        self.now = end


@pytest.fixture
def remote(monkeypatch):
    clock = Clock()
    monkeypatch.setattr(quirk, "monotonic", clock)
    monkeypatch.setattr(quirk, "call_later", clock.call_later)
    commissioning, rest = GPCommissioningPayload.deserialize(COMMISSIONING)
    assert rest == b""
    assert commissioning.device_id == DeviceID.GenericSwitch
    assert commissioning.switch_information.configuration.num_contacts == 8
    gpd = GreenPowerDevice(
        MagicMock(), application_id=ApplicationID.SrcID, src_id=SrcID(0x0EF62815)
    )
    gpd.device_id = commissioning.device_id
    gpd.gpd_manufacturer_id = commissioning.manufacturer_id
    gpd.gpd_model_id = commissioning.model_id
    entry = DEVICE_REGISTRY.match_green_power_entry(gpd)
    assert entry is not None
    zha_device = entry.zha_device_factory(DEVICE_REGISTRY.resolve(gpd), MagicMock())
    assert zha_device.name == "LeTV GP remote"
    entities = {e.unique_id.rsplit("-", 1)[1]: e for e in zha_device.discover_entities()}
    assert set(entities) == {*NAMES, "battery", "battery_voltage"}
    events = {name: [] for name in NAMES}
    for name, entity in entities.items():
        entity.on_add()
        if name in events:
            entity.on_event(
                EntityEventTriggeredEvent.event,
                lambda e, n=name: events[n].append(
                    (e.triggered.event_type, e.triggered.event_attributes)
                ),
            )
    return gpd, entities, events, clock


def contacts(gpd, command_id, mask):
    payload, rest = GPContactStatusPayload.deserialize(bytes([mask]))
    assert rest == b""
    gpd.emit(
        GreenPowerCommandReceived.event_type,
        GreenPowerCommandReceived(
            device_ieee="", endpoint_id=0, command_id=command_id, command=payload
        ),
    )


def press(gpd, mask):
    contacts(gpd, GPDCommandID.Press8BitVector, mask)


def release(gpd, mask):
    contacts(gpd, GPDCommandID.Release8BitVector, mask)


def types(events, name):
    return [event_type for event_type, _ in events[name]]


def test_registry_and_names(remote):
    _, entities, _, _ = remote
    assert [entities[n].fallback_name for n in NAMES] == [
        "Back", "Ring bottom", "Ring right", "Ring top",
        "Ring left", "Mic", "Play/Pause", "Center",
    ]
    # HA ZHA ignores fallback_name for an entity with a device class unless
    # it also has a translation key HA has no translation for.
    assert [entities[n].translation_key for n in NAMES] == [
        f"letv_{n}" for n in NAMES
    ]
    assert entities["battery_voltage"].translation_key == "letv_battery_voltage"
    for name in NAMES:
        assert entities[name].event_types == [
            ButtonEventType.PRESS_START,
            ButtonEventType.PRESS_END,
            ButtonEventType.LONG_PRESS_START,
            ButtonEventType.LONG_PRESS_END,
            ButtonEventType.SINGLE_PRESS,
            ButtonEventType.MULTI_PRESS,
        ]
    gpd = GreenPowerDevice(
        MagicMock(), application_id=ApplicationID.SrcID, src_id=SrcID(1)
    )
    gpd.device_id = DeviceID.GenericSwitch
    gpd.gpd_manufacturer_id = quirk.LETV_GP_MANUFACTURER_ID
    gpd.gpd_model_id = 0xF400
    assert DEVICE_REGISTRY.match_green_power_entry(gpd) is None


@pytest.mark.parametrize("bit", range(8))
def test_single_and_long_press(remote, bit):
    gpd, _, events, clock = remote
    name = NAMES[bit]
    # Binary-exact times, so the window and threshold edges are exact.
    press(gpd, 1 << bit)
    clock.advance(0.25)
    release(gpd, 0)
    clock.advance(0.375)
    assert types(events, name) == ["press_start", "press_end"]
    clock.advance(0.125)
    assert types(events, name)[-1] == "single_press"
    press(gpd, 1 << bit)
    clock.advance(0.875)
    assert types(events, name)[-1] == "press_start"
    clock.advance(0.125)
    assert types(events, name)[-1] == "long_press_start"
    clock.advance(0.5)
    release(gpd, 0)
    clock.advance(2)
    assert events[name] == [
        ("press_start", {}),
        ("press_end", {"duration": 0.25}),
        ("single_press", {}),
        ("press_start", {}),
        ("long_press_start", {}),
        ("long_press_end", {"duration": 1.5}),
    ]
    assert all(events[n] == [] for n in NAMES if n != name)
    assert clock.pending() == []


def test_multi_press(remote):
    gpd, _, events, clock = remote
    for _ in range(3):
        press(gpd, 0x01)
        clock.advance(0.1)
        release(gpd, 0x00)
        clock.advance(0.45)
    assert types(events, "back") == ["press_start", "press_end"] * 3
    clock.advance(0.05)
    assert events["back"][-1] == ("multi_press", {"press_count": 3})

    # A long press ends the count at its hold threshold and never counts.
    events["back"].clear()
    press(gpd, 0x01)
    clock.advance(0.1)
    release(gpd, 0x00)
    clock.advance(0.2)
    press(gpd, 0x01)
    clock.advance(1.0)
    release(gpd, 0x00)
    clock.advance(1)
    assert types(events, "back") == [
        "press_start", "press_end", "press_start",
        "single_press", "long_press_start", "long_press_end",
    ]

    # Without the hold timer (a busy loop), the release still pairs the hold.
    events["back"].clear()
    press(gpd, 0x01)
    clock.now += 1.2
    for timer in clock.pending():
        timer.cancel()
    release(gpd, 0x00)
    assert events["back"] == [
        ("press_start", {}),
        ("long_press_start", {}),
        ("long_press_end", {"duration": 1.2}),
    ]


def test_duplicates_and_multi_bit_masks(remote):
    gpd, entities, events, clock = remote
    press(gpd, 0x20)  # Mic
    press(gpd, 0x20)  # repeated frame, ignored
    clock.advance(0.5)
    press(gpd, 0xA0)  # Center joins: the Mic + OK combo
    press(gpd, 0xA0)
    clock.advance(3)
    release(gpd, 0x80)  # Mic released after 3.5 s
    release(gpd, 0x80)
    clock.advance(0.2)
    release(gpd, 0x00)  # Center released after 3.2 s
    release(gpd, 0x00)
    clock.advance(1)
    for name in ("mic", "center"):
        assert types(events, name) == ["press_start", "long_press_start", "long_press_end"]
    assert events["mic"][2][1] == {"duration": 3.5}
    assert events["center"][2][1] == {"duration": 3.2}
    assert all(events[n] == [] for n in NAMES if n not in ("mic", "center"))

    # A release frame for a button never seen pressed fires nothing, a press
    # with several new bits presses all of them.
    release(gpd, 0x00)
    press(gpd, 0x03)
    clock.advance(0.1)
    release(gpd, 0x00)
    clock.advance(0.5)
    for name in ("back", "ring_bottom"):
        assert types(events, name) == ["press_start", "press_end", "single_press"]

    # Removing the entity cancels its timers.
    press(gpd, 0x01)
    assert len(clock.pending()) == 1
    asyncio.run(entities["back"].on_remove())
    assert clock.pending() == []


def test_battery_reports(remote):
    gpd, entities, events, _ = remote
    sensors = {n: entities[n] for n in ("battery", "battery_voltage")}
    assert {n: e.native_value for n, e in sensors.items()} == {
        "battery": None, "battery_voltage": None
    }
    states = {n: [] for n in sensors}
    for name, entity in sensors.items():
        entity.on_event(STATE_CHANGED, states[name].append)

    report, rest = GPAttributeReportingPayload.deserialize(ATTRIBUTE_REPORT)
    assert rest == b""
    event = GreenPowerCommandReceived(
        device_ieee="", endpoint_id=0,
        command_id=GPDCommandID.AttributeReporting, command=report,
    )
    gpd.emit(GreenPowerCommandReceived.event_type, event)
    assert {n: e.native_value for n, e in sensors.items()} == {
        "battery": 90.0, "battery_voltage": 3.0
    }
    # Every report emits a state event, also with unchanged values.
    gpd.emit(GreenPowerCommandReceived.event_type, event)
    assert {n: len(s) for n, s in states.items()} == {"battery": 2, "battery_voltage": 2}

    report, rest = GPMultiClusterReportingPayload.deserialize(MULTI_REPORT)
    assert rest == b""
    gpd.emit(
        GreenPowerCommandReceived.event_type,
        GreenPowerCommandReceived(
            device_ieee="", endpoint_id=0,
            command_id=GPDCommandID.MultiClusterReporting, command=report,
        ),
    )
    assert {n: e.native_value for n, e in sensors.items()} == {
        "battery": 100.0, "battery_voltage": 3.0
    }
    assert all(e == [] for e in events.values())
