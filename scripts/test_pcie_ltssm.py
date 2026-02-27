#!/usr/bin/env python3

#
# This file is part of LitePCIe.
#
# Copyright (c) 2022 Sylvain Munaut <tnt@246tNt.com>
# SPDX-License-Identifier: BSD-2-Clause

import os
import argparse
import socket

from litex import RemoteClient

# PCIe LTSSM Dictionary ----------------------------------------------------------------------------

PCIE_LTSSM = {
    0x00 : "Detect Quiet",
    0x01 : "Detect Quiet",
    0x02 : "Detect Active",
    0x03 : "Detect Active",
    0x04 : "Polling Active",
    0x05 : "Polling Configuration",
    0x06 : "Polling Compliance, Pre_Send_EIOS",
    0x07 : "Polling Compliance, Pre_Timeout",
    0x08 : "Polling Compliance, Send_Pattern",
    0x09 : "Polling Compliance, Post_Send_EIOS",
    0x0A : "Polling Compliance, Post_Timeout",
    0x0B : "Configuration Linkwidth, State 0",
    0x0C : "Configuration Linkwidth, State 1",
    0x0D : "Configuration Linkwidth, Accept 0",
    0x0E : "Configuration Linkwidth, Accept 1",
    0x0F : "Configuration Lanenum Wait",
    0x10 : "Configuration Lanenum, Accept",
    0x11 : "Configuration Complete x1",
    0x12 : "Configuration Complete x2",
    0x13 : "Configuration Complete x4",
    0x14 : "Configuration Complete x8",
    0x15 : "Configuration Idle",
    0x16 : "L0",
    0x17 : "L1 Entry0",
    0x18 : "L1 Entry1",
    0x19 : "L1 Entry2 (also used for the L2/L3 Ready pseudo state)",
    0x1A : "L1 Idle",
    0x1B : "L1 Exit",
    0x1C : "Recovery Rcvrlock",
    0x1D : "Recovery Rcvrcfg",
    0x1E : "Recovery Speed_0",
    0x1F : "Recovery Speed_1",
    0x20 : "Recovery Idle",
    0x21 : "Hot Reset",
    0x22 : "Disabled Entry 0",
    0x23 : "Disabled Entry 1",
    0x24 : "Disabled Entry 2",
    0x25 : "Disabled Idle",
    0x26 : "Root Port, Configuration, Linkwidth State 0",
    0x27 : "Root Port, Configuration, Linkwidth State 1",
    0x28 : "Root Port, Configuration, Linkwidth State 2",
    0x29 : "Root Port, Configuration, Link Width Accept 0",
    0x2A : "Root Port, Configuration, Link Width Accept 1",
    0x2B : "Root Port, Configuration, Lanenum_Wait",
    0x2C : "Root Port, Configuration, Lanenum_Accept",
    0x2D : "Timeout To Detect",
    0x2E : "Loopback Entry0",
    0x2F : "Loopback Entry1",
    0x30 : "Loopback Active0",
    0x31 : "Loopback Exit0",
    0x32 : "Loopback Exit1",
    0x33 : "Loopback Master Entry0",
}

# PCIe LTSSM Tracer --------------------------------------------------------------------------------

def main():
    default_csr_csv = os.path.join(os.path.dirname(__file__), "csr.csv")
    parser = argparse.ArgumentParser(description="LitePCIe LTSSM tracer.")
    parser.add_argument("--csr-csv", default=default_csr_csv, help="CSR configuration file")
    parser.add_argument("--port",    default="1234",    help="Host bind port.")
    args = parser.parse_args()

    bus = RemoteClient(
        csr_csv = args.csr_csv,
        port    = int(args.port, 0)
    )
    bus.open()

    # Read history
    while True:
        v = bus.regs.pcie_phy_phy_ltssm_tracer_history.read()

        ltssm_new = (v >>  0) & 0x3f
        ltssm_old = (v >>  6) & 0x3f
        overflow  = (v >> 30) & 1
        valid     = (v >> 31) & 1

        if not valid:
            break

        print(f"[0x{ltssm_old:02x}] {PCIE_LTSSM.get(ltssm_old, 'reserved'):<32s} -> [0x{ltssm_new:02x}] {PCIE_LTSSM.get(ltssm_new, 'reserved'):<32s}{('[Overflow, possible unknown intermediate states]' if overflow else ''):s}")


if __name__ == "__main__":
    main()
