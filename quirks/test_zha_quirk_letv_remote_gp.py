"""Test the letv-remote-gp ZHA quirk against frames produced by include/gpd_frame.h."""

from unittest.mock import MagicMock

import pytest

from zha.application.platforms.event import EntityEventTriggeredEvent
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


class Clock:
    """Monotonic clock for the press durations."""

    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now


@pytest.fixture
def remote(monkeypatch):
    clock = Clock()
    monkeypatch.setattr(quirk, "monotonic", clock)
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
            "press", "release", "short_press", "long_press"
        ]
    gpd = GreenPowerDevice(
        MagicMock(), application_id=ApplicationID.SrcID, src_id=SrcID(1)
    )
    gpd.device_id = DeviceID.GenericSwitch
    gpd.gpd_manufacturer_id = quirk.LETV_GP_MANUFACTURER_ID
    gpd.gpd_model_id = 0xF400
    assert DEVICE_REGISTRY.match_green_power_entry(gpd) is None


@pytest.mark.parametrize("bit", range(8))
def test_short_and_long_press(remote, bit):
    gpd, _, events, clock = remote
    name = NAMES[bit]
    press(gpd, 1 << bit)
    clock.now += 0.3
    release(gpd, 0)
    clock.now += 2
    press(gpd, 1 << bit)
    clock.now += 1.0
    release(gpd, 0)
    assert events[name] == [
        ("press", {}),
        ("release", {"duration": 0.3}),
        ("short_press", {"duration": 0.3}),
        ("press", {}),
        ("release", {"duration": 1.0}),
        ("long_press", {"duration": 1.0}),
    ]
    assert all(events[n] == [] for n in NAMES if n != name)


def test_duplicates_and_multi_bit_masks(remote):
    gpd, _, events, clock = remote
    press(gpd, 0x20)  # Mic
    press(gpd, 0x20)  # repeated frame, ignored
    clock.now += 0.5
    press(gpd, 0xA0)  # Center joins: the Mic + OK combo
    press(gpd, 0xA0)
    clock.now += 3
    release(gpd, 0x80)  # Mic released after 3.5 s
    release(gpd, 0x80)
    clock.now += 0.2
    release(gpd, 0x00)  # Center released after 3.2 s
    release(gpd, 0x00)
    assert types(events, "mic") == ["press", "release", "long_press"]
    assert types(events, "center") == ["press", "release", "long_press"]
    assert events["mic"][1][1] == {"duration": 3.5}
    assert events["center"][1][1] == {"duration": 3.2}
    assert all(events[n] == [] for n in NAMES if n not in ("mic", "center"))

    # A release frame for a button never seen pressed fires nothing, a press
    # with several new bits presses all of them.
    release(gpd, 0x00)
    press(gpd, 0x03)
    clock.now += 0.1
    release(gpd, 0x00)
    assert types(events, "back") == ["press", "release", "short_press"]
    assert types(events, "ring_bottom") == ["press", "release", "short_press"]


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
