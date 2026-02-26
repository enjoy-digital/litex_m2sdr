#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2025-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from enum import IntEnum

from migen import *

from litex.gen import *

from litex.soc.interconnect import stream
from litex.soc.interconnect.packet import Header, HeaderField, Packetizer

from liteeth.frontend.stream import LiteEthStream2UDPTX


class VRTBool(IntEnum):
    DISABLED = 0x0
    ENABLED = 0x1


class VRTPacketType(IntEnum):
    SIG_DATA_NO_STREAM_ID = 0x0
    SIG_DATA_WITH_STREAM_ID = 0x1
    EXT_DATA_NO_STREAM_ID = 0x2
    EXT_DATA_WITH_STREAM_ID = 0x3
    CONTEXT = 0x4
    COMMAND = 0x5
    EXT_COMMAND = 0x6


class VRTTSI(IntEnum):
    NONE = 0x0
    UTC = 0x1
    GPS = 0x2
    OTHER = 0x3


class VRTTSF(IntEnum):
    NONE = 0x0
    SAMPLE_COUNT = 0x1
    REAL_TIME = 0x2
    FREE_RUNNING = 0x3


def _natural_to_header_fields(natural, total_bytes=4):
    fields = {}
    for name, (msb, lsb) in natural.items():
        width = msb - lsb + 1
        byte = (total_bytes - 1) - (msb // 8)
        offset = (msb % 8) - width + 1 if (msb // 8) == (lsb // 8) else 0
        fields[name] = HeaderField(byte, offset, width)
    return fields


def _common_header_fields():
    return _natural_to_header_fields({
        "packet_type": (31, 28),
        "c":          (27, 27),
        "t":          (26, 26),
        "r":          (25, 24),
        "tsi":        (23, 22),
        "tsf":        (21, 20),
        "packet_count": (19, 16),
        "packet_size":  (15, 0),
    }, total_bytes=4)


signal_header_fields = {
    **_common_header_fields(),
    "stream_id":     HeaderField(4, 0, 32),
    "timestamp_int": HeaderField(8, 0, 32),
    "timestamp_fra": HeaderField(12, 0, 64),
}

signal_header_length = 20  # bytes (5x32-bit words)
signal_header = Header(signal_header_fields, length=signal_header_length, swap_field_bytes=True)


def vrt_signal_packet_description(data_width):
    return stream.EndpointDescription(
        payload_layout=[("data", data_width)],
        param_layout=signal_header.get_layout(),
    )


def vrt_signal_packet_user_description(data_width):
    return stream.EndpointDescription(
        payload_layout=[
            ("data", data_width),
            ("data_words", 16),
        ],
        param_layout=[
            ("stream_id", 32),
            ("timestamp_int", 32),
            ("timestamp_fra", 64),
        ],
    )


class VRTSignalPacketInserter(LiteXModule):
    def __init__(self, data_width=32):
        self.sink = sink = stream.Endpoint(vrt_signal_packet_user_description(data_width))
        self.source = source = stream.Endpoint([("data", data_width)])

        packet_count = Signal(4)

        self.packetizer = packetizer = Packetizer(
            sink_description=vrt_signal_packet_description(data_width),
            source_description=[("data", data_width)],
            header=signal_header,
        )

        self.comb += [
            sink.connect(packetizer.sink, omit={"data_words"}),
            packetizer.sink.packet_type.eq(VRTPacketType.SIG_DATA_WITH_STREAM_ID),
            packetizer.sink.c.eq(VRTBool.DISABLED),
            packetizer.sink.t.eq(VRTBool.DISABLED),
            packetizer.sink.r.eq(0),
            packetizer.sink.tsi.eq(VRTTSI.UTC),
            packetizer.sink.tsf.eq(VRTTSF.REAL_TIME),
            packetizer.sink.packet_count.eq(packet_count),
            packetizer.sink.packet_size.eq(sink.data_words + signal_header_length // 4),
            packetizer.source.connect(source),
        ]

        self.sync += If(source.valid & source.ready & source.last,
            packet_count.eq(packet_count + 1)
        )


class VRTSignalPacketStreamer(LiteXModule):
    def __init__(self, udp_crossbar, ip_address, udp_port, data_width=32, with_csr=True):
        self.sink = stream.Endpoint(vrt_signal_packet_user_description(data_width))

        vrt_streamer_port = udp_crossbar.get_port(udp_port, dw=data_width, cd="sys")
        self.vrt_streamer = LiteEthStream2UDPTX(
            ip_address=ip_address,
            udp_port=udp_port,
            fifo_depth=1024,
            data_width=data_width,
            with_csr=with_csr,
        )
        self.vrt_inserter = VRTSignalPacketInserter(data_width=data_width)

        self.comb += [
            self.sink.connect(self.vrt_inserter.sink),
            self.vrt_inserter.source.connect(self.vrt_streamer.sink),
            self.vrt_streamer.source.connect(vrt_streamer_port.sink),
        ]

