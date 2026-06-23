#!/usr/bin/env python3
#
# SPDX-License-Identifier: BSD-2-Clause

import socket
import struct
import threading
import time


CSR_CTRL_SCRATCH_ADDR = 0x0004
CSR_XADC_TEMPERATURE_ADDR = 0x2000
CSR_XADC_VCCINT_ADDR = 0x2004
CSR_XADC_VCCAUX_ADDR = 0x2008
CSR_XADC_VCCBRAM_ADDR = 0x200C
CSR_DNA_ID_ADDR = 0x2800
CSR_IDENTIFIER_MEM_BASE = 0x4000
CSR_CAPABILITY_API_VERSION_ADDR = 0x6800
CSR_CAPABILITY_FEATURES_ADDR = 0x6804
CSR_CAPABILITY_PCIE_CONFIG_ADDR = 0x6808
CSR_CAPABILITY_ETH_CONFIG_ADDR = 0x680C
CSR_CAPABILITY_SATA_CONFIG_ADDR = 0x6810
CSR_CAPABILITY_BOARD_INFO_ADDR = 0x6814
CSR_SI5351_BASE = 0xA000
CSR_AD9361_CONFIG_ADDR = 0xC000
CSR_AD9361_SPI_STATUS_ADDR = 0xC014
CSR_AD9361_SPI_MOSI_ADDR = 0xC018
CSR_AD9361_SPI_MISO_ADDR = 0xC01C
CSR_TIME_GEN_READ_TIME_ADDR = 0x8804
CSR_ICAP_DATA_ADDR = 0x1004
CSR_ICAP_DONE_ADDR = 0x100C


class FakeLiteEthTarget:
    """Small Etherbone-over-UDP target for host-stack tests.

    The real LiteEth target and the host both use the same UDP port. The host
    sends requests from an ephemeral TX socket and listens for replies on the
    Etherbone port, so replies are sent back to the peer IP on the target port
    rather than to the request source port.
    """

    def __init__(self, host="127.0.0.2", port=0):
        self.host = host
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        self.sock.bind((host, port))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self._stop = threading.Event()
        self._thread = None
        self.regs = {}
        self._init_registers()

    def _init_registers(self):
        self.regs[CSR_CTRL_SCRATCH_ADDR] = 0x5A5AA5A5
        self.regs[CSR_XADC_TEMPERATURE_ADDR] = int(((35.0 + 273.15) * 4096) / 503.975)
        self.regs[CSR_XADC_VCCINT_ADDR] = int((1.00 / 3.0) * 4096)
        self.regs[CSR_XADC_VCCAUX_ADDR] = int((1.80 / 3.0) * 4096)
        self.regs[CSR_XADC_VCCBRAM_ADDR] = int((1.00 / 3.0) * 4096)
        self.regs[CSR_DNA_ID_ADDR + 0] = 0x01234567
        self.regs[CSR_DNA_ID_ADDR + 4] = 0x89ABCDEF
        self.regs[CSR_CAPABILITY_API_VERSION_ADDR] = 0x00010000
        self.regs[CSR_CAPABILITY_FEATURES_ADDR] = 1 << 1  # Ethernet.
        self.regs[CSR_CAPABILITY_PCIE_CONFIG_ADDR] = 0
        self.regs[CSR_CAPABILITY_ETH_CONFIG_ADDR] = 0
        self.regs[CSR_CAPABILITY_SATA_CONFIG_ADDR] = 0
        self.regs[CSR_CAPABILITY_BOARD_INFO_ADDR] = 1  # Baseboard variant.
        self.regs[CSR_SI5351_BASE] = 0x5  # Mark old/non-LiteI2C gateware.
        self.regs[CSR_AD9361_CONFIG_ADDR] = 0
        self.regs[CSR_AD9361_SPI_STATUS_ADDR] = 1
        self.regs[CSR_AD9361_SPI_MISO_ADDR] = 0
        self.regs[CSR_ICAP_DATA_ADDR] = 0
        self.regs[CSR_ICAP_DONE_ADDR] = 1

        ident = b"LiteX-M2SDR Fake Ethernet Target\n\0"
        for index, value in enumerate(ident):
            self.regs[CSR_IDENTIFIER_MEM_BASE + 4 * index] = value

    def start(self):
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)
        self.sock.close()

    def __enter__(self):
        return self.start()

    def __exit__(self, exc_type, exc, tb):
        self.stop()

    def _serve(self):
        while not self._stop.is_set():
            try:
                pkt, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break

            if addr[1] == self.port:
                continue
            if len(pkt) != 20 or pkt[0:2] != b"No":
                continue
            if pkt[2] != 0x10 or pkt[3] != 0x44:
                continue

            write_count = pkt[10]
            read_count = pkt[11]
            if write_count == 1 and read_count == 0:
                reg = struct.unpack(">I", pkt[12:16])[0]
                value = struct.unpack(">I", pkt[16:20])[0]
                self._write(reg, value)
            elif write_count == 0 and read_count == 1:
                reg = struct.unpack(">I", pkt[16:20])[0]
                self._reply_read(addr[0], reg)

    def _write(self, reg, value):
        self.regs[reg] = value & 0xFFFFFFFF
        if reg == CSR_AD9361_SPI_MOSI_ADDR:
            ad9361_reg = (value >> 8) & 0x7FFF
            is_write = (value & 0x800000) != 0
            if not is_write and ad9361_reg == 0x037:
                self.regs[CSR_AD9361_SPI_MISO_ADDR] = 0x0A

    def _read(self, reg):
        if reg == CSR_TIME_GEN_READ_TIME_ADDR or reg == CSR_TIME_GEN_READ_TIME_ADDR + 4:
            now = time.time_ns()
            return (now >> 32) & 0xFFFFFFFF if reg == CSR_TIME_GEN_READ_TIME_ADDR else now & 0xFFFFFFFF
        return self.regs.get(reg, 0)

    def _reply_read(self, peer_ip, reg):
        value = self._read(reg)
        reply = bytearray(20)
        reply[0:4] = b"\x4e\x6f\x10\x44"
        reply[9] = 0x0F
        reply[10] = 1
        reply[11] = 0
        reply[12:16] = struct.pack(">I", reg)
        reply[16:20] = struct.pack(">I", value)
        self.sock.sendto(reply, (peer_ip, self.port))
