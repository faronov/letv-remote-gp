"""ZHA Green Power quirk for the LeTV 8-button remote GPD firmware (letv-remote-gp).

The GPD commissions as a Generic Switch (DeviceID 0x07) with manufacturer 0x6D6B,
model 0xF500, eight push-button contacts (bit n is button n + 1) and the Power
Configuration server cluster. Every button change is a level-3 secured 8-bit vector
Press (0x69, contacts added) or Release (0x6A, contacts removed) frame that carries
the contacts closed now; the battery comes as Attribute Reporting (0x0001/0x0020 in
100 mV, 0x0001/0x0021 in 0.5 %) after commissioning and every 6 h.

Each button is an event entity with the standard button event types
(home-assistant/architecture#1502):

- press_start: the button went down;
- press_end: released less than 1 s after press_start (data: duration);
- long_press_start: still down 1 s after press_start, once per hold;
- long_press_end: released after long_press_start (data: duration);
- single_press: one short press and no new press within 0.5 s of its release;
- multi_press: two or more short presses, each within 0.5 s of the release before
  (data: press_count); a long press ends the sequence and never counts.

The GPD reports only the contact edges, so the quirk resolves the gestures with
timers, which the standard leaves to the device. single_press and multi_press come
0.5 s after the last release. The per-button state follows the contact bitmap of
each frame, so a mask that repeats the known state fires nothing. Holding Mic + OK
(buttons 6 and 8) is the GPD's own commissioning (3 s) and decommissioning (10 s)
gesture; their events still fire.

Copy to <HA config>/custom_zha_quirks/ (requires ZHA with Green Power support).
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import time
from typing import Any

from zha.application.platforms import EntityStateChangedEvent
from zha.application.platforms.event import BaseEvent
from zha.application.platforms.event.const import EventDeviceClass
from zha.application.platforms.sensor import BaseSensor
from zha.application.platforms.sensor.const import SensorDeviceClass, SensorStateClass
from zha.const import STATE_CHANGED
from zha.units import PERCENTAGE, UnitOfElectricPotential
from zigpy.device import GreenPowerCommandReceived
from zigpy.zgp.types import DeviceID, GPDCommandID

from zhaquirks.builder import GreenPowerQuirkBuilder

LETV_GP_MANUFACTURER_ID = 0x6D6B
LETV_GP_MODEL_ID = 0xF500
LONG_PRESS_S = 1.0
MULTI_PRESS_S = 0.5
# Contact bit n: button n + 1 (P0.n).
BUTTONS = (
    ("back", "Back"),
    ("ring_bottom", "Ring bottom"),
    ("ring_right", "Ring right"),
    ("ring_top", "Ring top"),
    ("ring_left", "Ring left"),
    ("mic", "Mic"),
    ("play_pause", "Play/Pause"),
    ("center", "Center"),
)
CONTACT_COMMANDS = (GPDCommandID.Press8BitVector, GPDCommandID.Release8BitVector)

# The clock for press durations and the timer scheduler, replaceable in tests.
monotonic = time.monotonic


def call_later(delay: float, callback: Callable[[], None]) -> asyncio.TimerHandle:
    """Run callback after delay seconds on the running event loop."""
    return asyncio.get_running_loop().call_later(delay, callback)


class LetvButtonEvent(BaseEvent):
    """One button of the remote, from the contact bitmap of every Press/Release."""

    _bit: int
    _attr_device_class = EventDeviceClass.BUTTON
    _attr_event_types = [
        "press_start",
        "press_end",
        "long_press_start",
        "long_press_end",
        "single_press",
        "multi_press",
    ]

    def __init__(self, device: Any, **kwargs: Any) -> None:
        """Initialize the button as released."""
        self._pressed_at: float | None = None
        self._held = False
        self._count = 0
        self._hold_timer: asyncio.TimerHandle | None = None
        self._count_timer: asyncio.TimerHandle | None = None
        super().__init__(device, **kwargs)

    def on_add(self) -> None:
        """Subscribe to the commands sent by the GPD."""
        super().on_add()
        self._on_remove_callbacks.append(
            self.device.device.on_event(
                GreenPowerCommandReceived.event_type, self._handle_gp_command
            )
        )
        self._on_remove_callbacks.append(self._cancel_timers)

    def _cancel_timers(self) -> None:
        for timer in (self._hold_timer, self._count_timer):
            if timer is not None:
                timer.cancel()
        self._hold_timer = self._count_timer = None

    def _handle_gp_command(self, event: GreenPowerCommandReceived) -> None:
        if event.command is None or event.command_id not in CONTACT_COMMANDS:
            return
        closed = bool(int(event.command.contact_status) >> self._bit & 1)
        if closed and self._pressed_at is None:
            self._press()
        elif not closed and self._pressed_at is not None:
            self._release()

    def _press(self) -> None:
        self._cancel_timers()
        self._pressed_at = monotonic()
        self._held = False
        self._trigger_event("press_start")
        self._hold_timer = call_later(LONG_PRESS_S, self._hold)

    def _hold(self) -> None:
        self._hold_timer = None
        if self._pressed_at is None or self._held:
            return
        self._held = True
        self._finish_count()
        self._trigger_event("long_press_start")

    def _release(self) -> None:
        self._cancel_timers()
        duration = monotonic() - self._pressed_at
        self._pressed_at = None
        attributes = {"duration": round(duration, 3)}
        if not self._held and duration >= LONG_PRESS_S:
            # The hold timer did not run in time: keep long_press_start paired.
            self._held = True
            self._finish_count()
            self._trigger_event("long_press_start")
        if self._held:
            self._trigger_event("long_press_end", attributes)
            return
        self._trigger_event("press_end", attributes)
        self._count += 1
        self._count_timer = call_later(MULTI_PRESS_S, self._finish_count)

    def _finish_count(self) -> None:
        """Report the counted short presses: single_press or multi_press."""
        self._count_timer = None
        count, self._count = self._count, 0
        if count == 1:
            self._trigger_event("single_press")
        elif count > 1:
            self._trigger_event("multi_press", {"press_count": count})


class LetvGreenPowerSensor(BaseSensor):
    """Sensor updated from one ZCL attribute in the GPD attribute reports."""

    _cluster_id: int
    _attribute_id: int
    _invalid: int
    _scale: int
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, device: Any, **kwargs: Any) -> None:
        """Initialize the sensor without a value."""
        self._value: float | None = None
        super().__init__(device, **kwargs)

    @property
    def native_value(self) -> float | None:
        """Return the last reported value."""
        return self._value

    def on_add(self) -> None:
        """Subscribe to the commands sent by the GPD."""
        super().on_add()
        self._on_remove_callbacks.append(
            self.device.device.on_event(
                GreenPowerCommandReceived.event_type, self._handle_gp_command
            )
        )

    def _handle_gp_command(self, event: GreenPowerCommandReceived) -> None:
        if event.command is None:
            return
        if event.command_id == GPDCommandID.MultiClusterReporting:
            records = [(r.cluster_id, r.attribute) for r in event.command.reports]
        elif event.command_id == GPDCommandID.AttributeReporting:
            records = [(event.command.cluster_id, a) for a in event.command.attributes]
        else:
            return
        for cluster_id, attribute in records:
            if cluster_id == self._cluster_id and attribute.attrid == self._attribute_id:
                raw = attribute.value.value
                before = self.state
                self._value = None if raw == self._invalid else raw / self._scale
                self._report(unchanged=self.state == before)

    def _report(self, unchanged: bool) -> None:
        """Emit the state for every report, so Home Assistant sees the device alive."""
        self.maybe_emit_state_changed_event()
        if not unchanged:
            return
        self.emit(
            STATE_CHANGED,
            EntityStateChangedEvent(**self.identifiers.__dict__, state_diff={}),
        )


class LetvBattery(LetvGreenPowerSensor):
    """Battery remaining (0x0001 BatteryPercentageRemaining, 0.5 %)."""

    _cluster_id, _attribute_id, _invalid, _scale = 0x0001, 0x0021, 0xFF, 2
    _attr_device_class = SensorDeviceClass.BATTERY
    _attr_native_unit_of_measurement = PERCENTAGE
    _attr_suggested_display_precision = 0


class LetvBatteryVoltage(LetvGreenPowerSensor):
    """Battery voltage (0x0001 BatteryVoltage, 100 mV)."""

    _cluster_id, _attribute_id, _invalid, _scale = 0x0001, 0x0020, 0xFF, 10
    _attr_device_class = SensorDeviceClass.VOLTAGE
    _attr_native_unit_of_measurement = UnitOfElectricPotential.VOLT
    _attr_suggested_display_precision = 1


_builder = GreenPowerQuirkBuilder().applies_to(
    device_id=DeviceID.GenericSwitch,
    manufacturer_id=LETV_GP_MANUFACTURER_ID,
    model_id=LETV_GP_MODEL_ID,
)
for _bit, (_suffix, _name) in enumerate(BUTTONS):
    _builder = _builder.adds_entity(
        type(f"LetvButtonEvent{_bit + 1}", (LetvButtonEvent,), {"_bit": _bit}),
        unique_id_suffix=_suffix,
        fallback_name=_name,
        # HA uses fallback_name for an entity with a device class only when
        # its translation key has no HA translation.
        translation_key=f"letv_{_suffix}",
    )
(
    _builder.adds_entity(LetvBattery, unique_id_suffix="battery", fallback_name="Battery")
    .adds_entity(
        LetvBatteryVoltage,
        unique_id_suffix="battery_voltage",
        fallback_name="Battery voltage",
        translation_key="letv_battery_voltage",
    )
    .friendly_name(manufacturer="LeTV", model="GP remote")
    .add_to_registry()
)
