#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *

from litex.gen import *

from litex.soc.interconnect.csr import *

from liteeth.core.ptp import (
    PTP_MSG_ANNOUNCE,
    PTP_MSG_DELAY_RESP,
    PTP_MSG_FOLLOW_UP,
    PTP_MSG_PDELAY_RESP,
    PTP_MSG_PDELAY_RESP_FUP,
    PTP_MSG_SYNC,
)


class PTPIdentityTracker(LiteXModule):
    """Track the local and learned master PTP port identities."""

    def __init__(self, with_csr=True):
        self.local_port_id          = Signal(80)
        self.master_ip              = Signal(32)
        self.clear                  = Signal()

        self.event_valid            = Signal()
        self.event_msg_type         = Signal(4)
        self.event_ip               = Signal(32)
        self.event_source_port_id   = Signal(80)

        self.general_valid          = Signal()
        self.general_msg_type       = Signal(4)
        self.general_ip             = Signal(32)
        self.general_source_port_id = Signal(80)

        self.master_port_id         = Signal(80)
        self.update_count           = Signal(32)

        # # #

        learn_from_event = Signal()
        learn_from_general = Signal()
        event_type_ok = Signal()
        general_type_ok = Signal()

        self.comb += [
            event_type_ok.eq(
                (self.event_msg_type == PTP_MSG_SYNC) |
                (self.event_msg_type == PTP_MSG_PDELAY_RESP)
            ),
            general_type_ok.eq(
                (self.general_msg_type == PTP_MSG_ANNOUNCE) |
                (self.general_msg_type == PTP_MSG_FOLLOW_UP) |
                (self.general_msg_type == PTP_MSG_DELAY_RESP) |
                (self.general_msg_type == PTP_MSG_PDELAY_RESP_FUP)
            ),
            learn_from_event.eq(
                self.event_valid &
                event_type_ok &
                ((self.master_ip == 0) | (self.event_ip == self.master_ip))
            ),
            learn_from_general.eq(
                self.general_valid &
                general_type_ok &
                ((self.master_ip == 0) | (self.general_ip == self.master_ip))
            ),
        ]

        self.sync += [
            If(self.clear,
                self.master_port_id.eq(0),
                self.update_count.eq(0),
            ).Elif(learn_from_general,
                self.master_port_id.eq(self.general_source_port_id),
                self.update_count.eq(self.update_count + 1),
            ).Elif(learn_from_event,
                self.master_port_id.eq(self.event_source_port_id),
                self.update_count.eq(self.update_count + 1),
            )
        ]

        if with_csr:
            self.add_csr()

    def add_csr(self):
        self._local_clock_id = CSRStatus(64, description="Local PTP clockIdentity (upper 64 bits of sourcePortIdentity).")
        self._local_port_number = CSRStatus(16, description="Local PTP portNumber (lower 16 bits of sourcePortIdentity).")
        self._master_clock_id = CSRStatus(64, description="Learned master clockIdentity (upper 64 bits of sourcePortIdentity).")
        self._master_port_number = CSRStatus(16, description="Learned master portNumber (lower 16 bits of sourcePortIdentity).")
        self._update_count = CSRStatus(32, description="Number of master sourcePortIdentity updates.")

        self.comb += [
            self._local_port_number.status.eq(self.local_port_id[0:16]),
            self._local_clock_id.status.eq(self.local_port_id[16:80]),
            self._master_port_number.status.eq(self.master_port_id[0:16]),
            self._master_clock_id.status.eq(self.master_port_id[16:80]),
            self._update_count.status.eq(self.update_count),
        ]
