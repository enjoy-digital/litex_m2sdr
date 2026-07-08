#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect import stream

from litepcie.common import *

# Constants / Helpers ------------------------------------------------------------------------------

_16_BIT_MODE = 0
_8_BIT_MODE  = 1
_BFP8_MODE   = 2

BFP8_HEADER_MAGIC    = 0x38504642 # "BFP8" as a little-endian 32-bit word.
BFP8_PAYLOAD_WORDS   = 127

def _sign_extend(data, nbits=16):
    return Cat(data, Replicate(data[-1], nbits - len(data)))

def _round_shift_12_to_8(module, data):
    # Signed round-to-nearest for Q11 -> Q7; negative ties round away from zero.
    rounded = Signal(13)
    module.comb += rounded.eq(_sign_extend(data, 13) + Mux(data[-1], 7, 8))
    return Mux((data[4:12] == 0x7f) & data[3], 0x7f, rounded[4:12])

def _round_shift_12(module, data, shift):
    if shift == 0:
        return data
    rounded = Signal(13)
    module.comb += rounded.eq(_sign_extend(data, 13) + Mux(data[-1], (1 << (shift - 1)) - 1, 1 << (shift - 1)))
    value_bits = 13 - shift
    return Cat(rounded[shift:13], Replicate(rounded[-1], 12 - value_bits))

def _clamp_12_to_8(data):
    return Mux(
        data[-1],
        Mux(data[7:12] != 0b11111, 0x80, data[0:8]),
        Mux(data[7:12] != 0,      0x7f, data[0:8]),
    )

def _bfp8_quantize(module, data, exponent):
    shifted = [Signal(12) for _ in range(5)]
    for shift in range(5):
        module.comb += shifted[shift].eq(_round_shift_12(module, data, shift))
    result = Signal(8)
    module.comb += Case(exponent, {
        shift: result.eq(_clamp_12_to_8(shifted[shift]))
        for shift in range(5)
    })
    return result

def _scale_i8_to_12(module, data, exponent):
    scaled = [Signal(12) for _ in range(5)]
    for shift in range(5):
        module.comb += scaled[shift].eq((_sign_extend(data, 12) << shift)[0:12])
    result = Signal(12)
    module.comb += Case(exponent, {
        shift: result.eq(scaled[shift])
        for shift in range(5)
    })
    return result

def _sample_abs(module, data):
    signed = Signal(13)
    value = Signal(13)
    module.comb += [
        signed.eq(_sign_extend(data, 13)),
        value.eq(Mux(signed[-1], -signed, signed)),
    ]
    return value

def _max4(module, values):
    max01 = Signal(13)
    max23 = Signal(13)
    maxv  = Signal(13)
    module.comb += [
        max01.eq(Mux(values[0] > values[1], values[0], values[1])),
        max23.eq(Mux(values[2] > values[3], values[2], values[3])),
        maxv.eq(Mux(max01 > max23, max01, max23)),
    ]
    return maxv

def _bfp8_exponent(max_abs):
    return Mux(max_abs <= (127 << 0), 0,
        Mux(max_abs <= (127 << 1), 1,
        Mux(max_abs <= (127 << 2), 2,
        Mux(max_abs <= (127 << 3), 3, 4))))

def _bfp8_header(exponent, payload_words, sequence):
    return Cat(
        Constant(BFP8_HEADER_MAGIC, 32),
        exponent,
        Constant(0, 4),
        Constant(payload_words, 8),
        sequence,
        Constant(1, 8),
    )

def _pack_bfp8_payload(module, word0, word1, exponent):
    fields = []
    for word in [word0, word1]:
        for i in range(4):
            fields.append(_bfp8_quantize(module, word[i*16:i*16 + 12], exponent))
    return Cat(*fields)

def _unpack_bfp8_payload(module, data, exponent, half):
    fields = []
    for i in range(4):
        scaled = _scale_i8_to_12(module, data[(half*4 + i)*8:(half*4 + i + 1)*8], exponent)
        fields.append(Cat(scaled, Constant(0, 4)))
    return Cat(*fields)

# AD9361 TX BitMode --------------------------------------------------------------------------------

class AD9361TXBitMode(LiteXModule):
    def __init__(self, bfp8_payload_words=BFP8_PAYLOAD_WORDS, wide=False):
        # Wide (Oversampling) variant: 128-bit stream = two (ia,qa,ib,qb) groups per word, so the
        # sys-domain word rate halves (2T2R @ 122.88 MSPS fits the 64-bit @ 125MHz fabric). Both
        # formats are normalized to two expanded 16-bit groups per 128-bit word; the rfic side
        # scatters 128->64 into the serializer. BFP8 is not supported in wide mode.
        if wide:
            self.sink   = sink   = stream.Endpoint(dma_layout(128))
            self.source = source = stream.Endpoint(dma_layout(128))
            self.mode   = mode   = Signal(2)

            # 16-bit: a 128-bit word already holds two 16-bit groups -> passthrough.
            self.comb += If(mode == _16_BIT_MODE, sink.connect(source))

            # 8-bit: a 128-bit word holds four sc8 groups (16 bytes). Split to two 64-bit halves
            # (two sc8 groups each), then sign-extend the 8 bytes to 8x16 = one 128-bit word.
            self.conv = conv = stream.Converter(128, 64)
            ext = [source.data[i*16+4:(i+1)*16].eq(_sign_extend(conv.source.data[i*8:(i+1)*8], 12))
                   for i in range(8)]
            self.comb += If(mode == _8_BIT_MODE,
                sink.connect(conv.sink),
                conv.source.connect(source, omit={"data"}),
                *ext,
            )
            return

        self.sink   = sink   = stream.Endpoint(dma_layout(64))
        self.source = source = stream.Endpoint(dma_layout(64))
        self.mode   = mode   = Signal(2)

        # # #

        # 16-bit mode.
        # ------------
        self.comb += If(mode == _16_BIT_MODE, sink.connect(source))

        # 8-bit mode.
        # -----------
        self.conv = conv = stream.Converter(64, 32)
        self.comb += If(mode == _8_BIT_MODE,
            sink.connect(conv.sink),
            conv.source.connect(source, omit={"data"}),
            source.data[0*16+4:1*16].eq(_sign_extend(conv.source.data[0*8:1*8], 12)),
            source.data[1*16+4:2*16].eq(_sign_extend(conv.source.data[1*8:2*8], 12)),
            source.data[2*16+4:3*16].eq(_sign_extend(conv.source.data[2*8:3*8], 12)),
            source.data[3*16+4:4*16].eq(_sign_extend(conv.source.data[3*8:4*8], 12)),
        )

        # BFP8 mode.
        # ----------
        bfp8_exponent      = Signal(4)
        bfp8_payload_count = Signal(max=bfp8_payload_words + 1)
        bfp8_payload_data  = Signal(64)
        self.fsm = fsm = FSM(reset_state="BFP8_HEADER")
        fsm.act("BFP8_HEADER",
            If(mode != _BFP8_MODE,
                NextValue(bfp8_payload_count, 0),
            ).Else(
                sink.ready.eq(1),
                If(sink.valid,
                    NextValue(bfp8_exponent, sink.data[32:36]),
                    NextValue(bfp8_payload_count, 0),
                    NextState("BFP8_PAYLOAD"),
                )
            )
        )
        fsm.act("BFP8_PAYLOAD",
            If(mode != _BFP8_MODE,
                NextState("BFP8_HEADER"),
            ).Else(
                sink.ready.eq(1),
                If(sink.valid,
                    NextValue(bfp8_payload_data, sink.data),
                    NextState("BFP8_OUT0"),
                )
            )
        )
        fsm.act("BFP8_OUT0",
            If(mode != _BFP8_MODE,
                NextState("BFP8_HEADER"),
            ).Else(
                source.valid.eq(1),
                source.data.eq(_unpack_bfp8_payload(self, bfp8_payload_data, bfp8_exponent, 0)),
                If(source.ready,
                    NextState("BFP8_OUT1"),
                )
            )
        )
        fsm.act("BFP8_OUT1",
            If(mode != _BFP8_MODE,
                NextState("BFP8_HEADER"),
            ).Else(
                source.valid.eq(1),
                source.data.eq(_unpack_bfp8_payload(self, bfp8_payload_data, bfp8_exponent, 1)),
                source.last.eq(bfp8_payload_count == (bfp8_payload_words - 1)),
                If(source.ready,
                    If(bfp8_payload_count == (bfp8_payload_words - 1),
                        NextState("BFP8_HEADER"),
                    ).Else(
                        NextValue(bfp8_payload_count, bfp8_payload_count + 1),
                        NextState("BFP8_PAYLOAD"),
                    )
                )
            )
        )

# AD9361 RX BitMode --------------------------------------------------------------------------------

class AD9361RXBitMode(LiteXModule):
    def __init__(self, bfp8_payload_words=BFP8_PAYLOAD_WORDS, wide=False):
        # Wide (Oversampling) variant: 128-bit stream = two 16-bit groups per word (the rfic side
        # gathers 64->128). 16-bit passes through; 8-bit rounds the 8 lanes to 8 bytes (two sc8
        # groups), then packs two such words into one 128-bit DMA word (four sc8 groups). BFP8 is
        # not supported in wide mode.
        if wide:
            self.sink   = sink   = stream.Endpoint(dma_layout(128))
            self.source = source = stream.Endpoint(dma_layout(128))
            self.mode   = mode   = Signal(2)

            self.comb += If(mode == _16_BIT_MODE, sink.connect(source))

            self.conv = conv = stream.Converter(64, 128)
            rnd = [conv.sink.data[i*8:(i+1)*8].eq(_round_shift_12_to_8(self, sink.data[i*16:i*16+12]))
                   for i in range(8)]
            self.comb += If(mode == _8_BIT_MODE,
                sink.connect(conv.sink, omit={"data"}),
                *rnd,
                conv.source.connect(source),
            )
            return

        self.sink   = sink   = stream.Endpoint(dma_layout(64))
        self.source = source = stream.Endpoint(dma_layout(64))
        self.mode   = mode   = Signal(2)

        # # #

        # 16-bit mode.
        # ------------
        self.comb += If(mode == _16_BIT_MODE, sink.connect(source))

        # 8-bit mode.
        # -----------
        self.conv = conv = stream.Converter(32, 64)
        self.comb += If(mode == _8_BIT_MODE,
            sink.connect(conv.sink, omit={"data"}),
            conv.sink.data[0*8:1*8].eq(_round_shift_12_to_8(self, sink.data[0*16:0*16+12])),
            conv.sink.data[1*8:2*8].eq(_round_shift_12_to_8(self, sink.data[1*16:1*16+12])),
            conv.sink.data[2*8:3*8].eq(_round_shift_12_to_8(self, sink.data[2*16:2*16+12])),
            conv.sink.data[3*8:4*8].eq(_round_shift_12_to_8(self, sink.data[3*16:3*16+12])),
            conv.source.connect(source),
        )

        # BFP8 mode.
        # ----------
        bfp8_input_words = bfp8_payload_words * 2
        self.bfp8_fifo = bfp8_fifo = stream.SyncFIFO(dma_layout(64), depth=bfp8_input_words, buffered=True)

        bfp8_collect_count = Signal(max=bfp8_input_words)
        bfp8_payload_count = Signal(max=bfp8_payload_words + 1)
        bfp8_max_abs       = Signal(13)
        bfp8_exponent      = Signal(4)
        bfp8_sequence      = Signal(8)
        bfp8_word0         = Signal(64)

        sample_abs = [
            _sample_abs(self, sink.data[i*16:i*16 + 12])
            for i in range(4)
        ]
        beat_max = _max4(self, sample_abs)
        next_max_abs = Signal(13)
        self.comb += next_max_abs.eq(Mux(bfp8_max_abs > beat_max, bfp8_max_abs, beat_max))

        self.bfp8_fsm = bfp8_fsm = FSM(reset_state="BFP8_COLLECT")
        bfp8_fsm.act("BFP8_COLLECT",
            If(mode != _BFP8_MODE,
                NextValue(bfp8_collect_count, 0),
                NextValue(bfp8_max_abs, 0),
            ).Else(
                sink.ready.eq(bfp8_fifo.sink.ready),
                bfp8_fifo.sink.valid.eq(sink.valid),
                bfp8_fifo.sink.data.eq(sink.data),
                If(sink.valid & sink.ready,
                    NextValue(bfp8_max_abs, next_max_abs),
                    If(bfp8_collect_count == (bfp8_input_words - 1),
                        NextValue(bfp8_collect_count, 0),
                        NextState("BFP8_EXP"),
                    ).Else(
                        NextValue(bfp8_collect_count, bfp8_collect_count + 1),
                    )
                )
            )
        )
        # Derive the block exponent from the REGISTERED max in its own cycle.
        # Folding it into the last collect beat placed sample-abs -> max4 ->
        # running-max -> the 4-deep exponent mux chain in one combinational cone off
        # the RX CDC output, which is timing-marginal at 125MHz sys_clk; splitting it
        # into two register-to-register hops closes it (one extra cycle per block).
        bfp8_fsm.act("BFP8_EXP",
            If(mode != _BFP8_MODE,
                NextState("BFP8_COLLECT"),
            ).Else(
                NextValue(bfp8_exponent, _bfp8_exponent(bfp8_max_abs)),
                NextValue(bfp8_max_abs, 0),
                NextState("BFP8_HEADER"),
            )
        )
        bfp8_fsm.act("BFP8_HEADER",
            If(mode != _BFP8_MODE,
                NextState("BFP8_COLLECT"),
            ).Else(
                source.valid.eq(1),
                source.first.eq(1),
                source.data.eq(_bfp8_header(bfp8_exponent, bfp8_payload_words, bfp8_sequence)),
                If(source.ready,
                    NextValue(bfp8_payload_count, 0),
                    NextValue(bfp8_sequence, bfp8_sequence + 1),
                    NextState("BFP8_READ0"),
                )
            )
        )
        bfp8_fsm.act("BFP8_READ0",
            If(mode != _BFP8_MODE,
                NextState("BFP8_COLLECT"),
            ).Else(
                bfp8_fifo.source.ready.eq(1),
                If(bfp8_fifo.source.valid,
                    NextValue(bfp8_word0, bfp8_fifo.source.data),
                    NextState("BFP8_PAYLOAD"),
                )
            )
        )
        bfp8_fsm.act("BFP8_PAYLOAD",
            If(mode != _BFP8_MODE,
                NextState("BFP8_COLLECT"),
            ).Else(
                source.valid.eq(bfp8_fifo.source.valid),
                source.data.eq(_pack_bfp8_payload(self, bfp8_word0, bfp8_fifo.source.data, bfp8_exponent)),
                source.last.eq(bfp8_payload_count == (bfp8_payload_words - 1)),
                bfp8_fifo.source.ready.eq(source.ready),
                If(source.valid & source.ready,
                    If(bfp8_payload_count == (bfp8_payload_words - 1),
                        NextState("BFP8_COLLECT"),
                    ).Else(
                        NextValue(bfp8_payload_count, bfp8_payload_count + 1),
                        NextState("BFP8_READ0"),
                    )
                )
            )
        )
