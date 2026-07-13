from migen.sim import run_simulation

from litex_m2sdr.gateware.etherbone import SharedLiteEthEtherbonePacketTX


def test_etherbone_reply_uses_request_source_port():
    dut = SharedLiteEthEtherbonePacketTX(udp_port=1234)
    observed = []

    def generator():
        yield dut.source.ready.eq(1)
        yield dut.sink.valid.eq(1)
        yield dut.sink.last.eq(1)
        yield dut.sink.last_be.eq(0xf)
        yield dut.sink.data.eq(0x11223344)
        yield dut.sink.length.eq(4)
        yield dut.sink.src_port.eq(49152)
        yield dut.sink.ip_address.eq(0xc0a80164)

        accepted = False
        for _ in range(32):
            if (yield dut.sink.valid) and (yield dut.sink.ready):
                accepted = True
                yield dut.sink.valid.eq(0)
            if (yield dut.source.valid):
                observed.append({
                    "src_port": (yield dut.source.src_port),
                    "dst_port": (yield dut.source.dst_port),
                    "ip":       (yield dut.source.ip_address),
                })
            yield
        assert accepted

    run_simulation(dut, generator())
    assert observed
    assert {beat["src_port"] for beat in observed} == {1234}
    assert {beat["dst_port"] for beat in observed} == {49152}
    assert {beat["ip"] for beat in observed} == {0xc0a80164}
