# SPDX-License-Identifier: BSD-3-Clause
"""The host test's GPDFs (build/host/test-gpd-frame with an argument) through
zigpy: the GPDF parser, the GP manager's commissioning security, a
GreenPowerDevice created from the Commissioning GPDF and its command schemas
for the secured Press, Release and battery frames.

Run after `make test`. ZIGPY_SRC may name a zigpy source tree to use instead
of the installed package (the test then checks that it was imported from
there); ZIGPY_EXPECT_VERSION checks the installed distribution's version."""
import importlib.metadata
import os
import subprocess
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

if os.environ.get("ZIGPY_SRC"):
    sys.path.insert(0, os.environ["ZIGPY_SRC"])

import zigpy  # noqa: E402
from zigpy.device import GreenPowerCommandReceived, GreenPowerDevice  # noqa: E402
from zigpy.zgp.commands import GPD_COMMAND_SCHEMAS, GPCommissioningPayload  # noqa: E402
from zigpy.zgp.manager import commissioning_security  # noqa: E402
from zigpy.zgp.types import (  # noqa: E402
    DeviceID,
    GPDCommandID,
    SecurityKeyType,
    SecurityLevel,
    SwitchType,
)
from zigpy.zgp.util import parse_gpdf  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
HOST_TEST = ROOT / "build" / "host" / "test-gpd-frame"
SRC_ID = 0x0EF62815
KEY = bytes(range(0xC0, 0xD0))
MAC_HEADER = 7


def frames():
    if not HOST_TEST.exists():
        pytest.skip("build/host/test-gpd-frame missing: run `make test` first")
    out = subprocess.run([str(HOST_TEST), "hex"], check=True, capture_output=True, text=True).stdout
    return {name: bytes.fromhex(value) for name, value in (line.split() for line in out.splitlines())}


@pytest.fixture(scope="module")
def gpdf():
    return frames()


def nwk(frame):
    # The MAC header carries the sequence number; zigpy parses from the NWK
    # frame control on (the radio strips the FCS).
    return parse_gpdf(frame[MAC_HEADER:])


def test_version():
    expected = os.environ.get("ZIGPY_EXPECT_VERSION")
    if expected:
        assert importlib.metadata.version("zigpy") == expected
    if os.environ.get("ZIGPY_SRC"):
        assert Path(zigpy.__file__).resolve().is_relative_to(Path(os.environ["ZIGPY_SRC"]).resolve())


def test_commissioning(gpdf):
    packet = nwk(gpdf["commissioning"])
    assert packet.src_id == SRC_ID
    assert packet.command_id == GPDCommandID.CommissioningRequest
    assert packet.security_level == SecurityLevel.NoSecurity
    comm, rest = GPCommissioningPayload.deserialize(packet.payload.serialize())
    assert rest == b""
    assert comm.device_id == DeviceID.GenericSwitch == 0x07
    assert comm.options.mac_seq_num_capability == 1
    assert comm.options.rx_on_capability == 0
    assert comm.options.fixed_location == 0
    assert comm.manufacturer_id == 0x6D6B
    assert comm.model_id == 0xF500
    assert comm.gpd_commands is None
    assert comm.cluster_counts.num_server == 1 and comm.cluster_counts.num_client == 0
    assert list(comm.server_clusters) == [0x0001]
    assert list(comm.client_clusters) == []
    assert comm.app_info.switch_info_present == 1
    switch = comm.switch_information
    assert switch.length == 2
    assert switch.configuration.num_contacts == 8
    assert switch.configuration.switch_type == SwitchType.Button
    assert switch.contact_status == 0
    assert comm.extended_options.security_level == SecurityLevel.Encrypted
    assert comm.extended_options.key_type == SecurityKeyType.IndividualKey
    assert comm.outgoing_counter == 0x400

    level, key_type, key = commissioning_security(SRC_ID, comm)
    assert level == SecurityLevel.Encrypted
    assert key_type == SecurityKeyType.IndividualKey
    assert key.serialize() == KEY


def device(gpdf):
    packet = nwk(gpdf["commissioning"])
    comm, _ = GPCommissioningPayload.deserialize(packet.payload.serialize())
    level, key_type, key = commissioning_security(SRC_ID, comm)
    gpd = GreenPowerDevice.from_commissioning(
        MagicMock(), packet, comm, security_level=level, security_key_type=key_type, security_key=key
    )
    events = []
    gpd.on_event(GreenPowerCommandReceived.event_type, events.append)
    return gpd, events


def test_device_and_commands(gpdf):
    gpd, events = device(gpdf)
    assert gpd.device_id == DeviceID.GenericSwitch
    assert gpd.gpd_manufacturer_id == 0x6D6B and gpd.gpd_model_id == 0xF500
    assert gpd.server_cluster_ids == [0x0001]
    assert gpd.switch_configuration.num_contacts == 8
    assert gpd.switch_configuration.switch_type == SwitchType.Button
    assert gpd.fixed_location is False

    # Counters 0x402, 0x403, 0x404 after the commissioning's 0x400; each
    # repetition of a GPDF is a duplicate.
    for name in ("secured-press", "secured-press", "secured-release", "secured-battery", "secured-battery"):
        gpd.packet_received(nwk(gpdf[name]))
    assert [e.command_id for e in events] == [
        GPDCommandID.Press8BitVector,
        GPDCommandID.Release8BitVector,
        GPDCommandID.AttributeReporting,
    ]
    assert GPDCommandID.Press8BitVector == 0x69 and GPDCommandID.Release8BitVector == 0x6A
    assert events[0].command.contact_status == 0xA0
    assert events[1].command.contact_status == 0x80
    report = events[2].command
    assert report.cluster_id == 0x0001
    assert [(a.attrid, a.value.value) for a in report.attributes] == [(0x0020, 30), (0x0021, 180)]


def test_tampered_frame_dropped(gpdf):
    gpd, events = device(gpdf)
    bad = bytearray(gpdf["secured-press"])
    bad[-1] ^= 1
    gpd.packet_received(nwk(bytes(bad)))
    assert events == []


def test_open_payloads(gpdf):
    for name, command, status in (
        ("press", GPDCommandID.Press8BitVector, 0xA0),
        ("release", GPDCommandID.Release8BitVector, 0x80),
        ("release-all", GPDCommandID.Release8BitVector, 0x00),
    ):
        packet = nwk(gpdf[name])
        assert packet.command_id == command
        payload, rest = GPD_COMMAND_SCHEMAS[command].deserialize(packet.payload.serialize())
        assert rest == b"" and payload.contact_status == status
    packet = nwk(gpdf["battery"])
    report, rest = GPD_COMMAND_SCHEMAS[packet.command_id].deserialize(packet.payload.serialize())
    assert rest == b"" and report.cluster_id == 0x0001


def test_decommissioning(gpdf):
    packet = nwk(gpdf["secured-decommissioning"])
    assert packet.security_level == SecurityLevel.Encrypted and packet.frame_counter == 0x12345678
