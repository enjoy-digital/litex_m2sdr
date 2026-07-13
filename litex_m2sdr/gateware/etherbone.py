#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""Etherbone frontend with per-request UDP reply routing.

The standard LiteEth Etherbone frontend sends every reply to its fixed service
port.  That requires all host clients to bind the same UDP port and makes two
independent processes race for replies.  This variant preserves the request's
source IP/port and returns each read/probe reply to that endpoint.
"""

from litex.gen import *

from litex.soc.interconnect import stream
from litex.soc.interconnect.packet import Arbiter, Dispatcher

from liteeth.common import (
    etherbone_magic,
    etherbone_packet_header,
    etherbone_record_header,
    etherbone_version,
    eth_etherbone_packet_user_description,
    eth_udp_user_description,
    reverse_bytes,
)
from liteeth.frontend.etherbone import (
    LiteEthEtherbonePacketPacketizer,
    LiteEthEtherbonePacketRX,
    LiteEthEtherboneProbe,
    LiteEthEtherboneRecordDepacketizer,
    LiteEthEtherboneRecordPacketizer,
    LiteEthEtherboneRecordReceiver,
    LiteEthEtherboneRecordSender,
    LiteEthEtherboneWishboneMaster,
)


class SharedLiteEthEtherbonePacketTX(LiteXModule):
    """Etherbone packet TX returning replies to the request source port."""

    def __init__(self, udp_port):
        self.sink   = sink   = stream.Endpoint(eth_etherbone_packet_user_description(32))
        self.source = source = stream.Endpoint(eth_udp_user_description(32))

        self.packetizer = packetizer = LiteEthEtherbonePacketPacketizer()
        self.comb += [
            sink.connect(packetizer.sink, keep={"valid", "last", "last_be", "ready", "data"}),
            sink.connect(packetizer.sink, keep={"pf", "pr", "nr"}),
            packetizer.sink.version.eq(etherbone_version),
            packetizer.sink.magic.eq(etherbone_magic),
            packetizer.sink.port_size.eq(32//8),
            packetizer.sink.addr_size.eq(32//8),
        ]
        self.fsm = fsm = FSM(reset_state="IDLE")
        fsm.act("IDLE",
            If(packetizer.source.valid,
                NextState("SEND")
            )
        )
        fsm.act("SEND",
            packetizer.source.connect(source),
            source.src_port.eq(udp_port),
            # Delta vs LiteEth's LiteEthEtherbonePacketTX: reply to the
            # requester's source port instead of the fixed service port.
            source.dst_port.eq(sink.src_port),
            source.ip_address.eq(sink.ip_address),
            source.length.eq(sink.length + etherbone_packet_header.length),
            If(source.valid & source.last & source.ready,
                NextState("IDLE")
            )
        )


class SharedLiteEthEtherbonePacket(LiteXModule):
    def __init__(self, udp, udp_port, cd="sys"):
        self.tx = tx = SharedLiteEthEtherbonePacketTX(udp_port)
        self.rx = rx = LiteEthEtherbonePacketRX(
            with_last_handler=(udp.crossbar.dw == 64)
        )
        port = udp.crossbar.get_port(udp_port, dw=32, cd=cd)
        self.comb += [
            tx.source.connect(port.sink),
            port.source.connect(rx.sink),
        ]
        self.sink, self.source = self.tx.sink, self.rx.source


class SharedLiteEthEtherboneRecord(LiteXModule):
    """Etherbone record path with ordered reply endpoint metadata."""

    def __init__(self, endianness="big", buffer_depth=4):
        self.sink   = sink   = stream.Endpoint(eth_etherbone_packet_user_description(32))
        self.source = source = stream.Endpoint(eth_etherbone_packet_user_description(32))

        self.depacketizer = depacketizer = LiteEthEtherboneRecordDepacketizer()
        self.receiver     = receiver     = LiteEthEtherboneRecordReceiver(buffer_depth)
        self.comb += sink.connect(depacketizer.sink)

        # A read response can be delayed by Wishbone arbitration. Queue the
        # requesting endpoint with the read record instead of keeping only a
        # single global "last client" value.
        metadata_layout = [
            ("ip_address", 32),
            ("src_port",   16),
        ]
        self.metadata_fifo = metadata_fifo = stream.SyncFIFO(
            metadata_layout, buffer_depth, buffered=True
        )
        first = Signal(reset=1)
        needs_metadata = Signal()
        can_accept = Signal()
        self.comb += [
            needs_metadata.eq(first & (depacketizer.source.rcount != 0)),
            can_accept.eq(~needs_metadata | metadata_fifo.sink.ready),
            depacketizer.source.connect(receiver.sink, omit={"valid", "ready"}),
            receiver.sink.valid.eq(depacketizer.source.valid & can_accept),
            depacketizer.source.ready.eq(receiver.sink.ready & can_accept),
            metadata_fifo.sink.valid.eq(
                depacketizer.source.valid & receiver.sink.ready & needs_metadata
            ),
            metadata_fifo.sink.ip_address.eq(sink.ip_address),
            metadata_fifo.sink.src_port.eq(sink.src_port),
        ]
        self.sync += If(depacketizer.source.valid & depacketizer.source.ready,
            first.eq(depacketizer.source.last)
        )
        if endianness == "big":
            self.comb += receiver.sink.data.eq(reverse_bytes(depacketizer.source.data))

        self.sender     = sender     = LiteEthEtherboneRecordSender(buffer_depth)
        self.packetizer = packetizer = LiteEthEtherboneRecordPacketizer()
        self.comb += [
            sender.source.connect(packetizer.sink),
            packetizer.source.connect(source),
            source.valid.eq(packetizer.source.valid & metadata_fifo.source.valid),
            packetizer.source.ready.eq(source.ready & metadata_fifo.source.valid),
            source.length.eq(etherbone_record_header.length +
                (sender.source.wcount != 0)*4 + sender.source.wcount*4 +
                (sender.source.rcount != 0)*4 + sender.source.rcount*4),
            source.ip_address.eq(metadata_fifo.source.ip_address),
            source.src_port.eq(metadata_fifo.source.src_port),
            metadata_fifo.source.ready.eq(source.valid & source.ready & source.last),
        ]
        if endianness == "big":
            self.comb += packetizer.sink.data.eq(reverse_bytes(sender.source.data))


class SharedLiteEthEtherbone(LiteXModule):
    """Wishbone-master Etherbone frontend safe for multiple UDP clients."""

    def __init__(self, udp, udp_port, buffer_depth=4, cd="sys"):
        self.packet = packet = SharedLiteEthEtherbonePacket(udp, udp_port, cd)
        self.probe  = probe  = LiteEthEtherboneProbe()
        self.record = record = SharedLiteEthEtherboneRecord(buffer_depth=buffer_depth)

        dispatcher = Dispatcher(packet.source, [probe.sink, record.sink])
        arbiter = Arbiter([probe.source, record.source], packet.sink)
        self.comb += dispatcher.sel.eq(~packet.source.pf)
        self.submodules += dispatcher, arbiter

        self.wishbone = LiteEthEtherboneWishboneMaster()
        self.comb += [
            record.receiver.source.connect(self.wishbone.sink),
            self.wishbone.source.connect(record.sender.sink),
        ]
