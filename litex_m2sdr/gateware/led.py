#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2024-2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

from migen import *
from migen.genlib.cdc import MultiReg

from litex.gen import *

# Constants ----------------------------------------------------------------------------------------

STATUS_LED_TICK_PERIOD_S         = 10e-3
STATUS_LED_BREATH_BITS           = 6
STATUS_LED_BREATH_RESET          = 4
STATUS_LED_HEARTBEAT_PERIOD      = 100
STATUS_LED_HEARTBEAT_PULSE0      = (0, 6)
STATUS_LED_HEARTBEAT_PULSE1      = (12, 18)
STATUS_LED_PPS_HOLD_TICKS        = 8
STATUS_LED_ACTIVITY_PERIOD_TICKS = 4
STATUS_LED_ACTIVITY_WIDTH_TICKS  = 1
STATUS_LED_BOOT_LEVEL            = 140
STATUS_LED_DISCOVERY_LEVEL       = 96
STATUS_LED_IDLE_BASE_LEVEL       = 10
STATUS_LED_ACTIVITY_LEVEL        = 208
STATUS_LED_DUPLEX_LEVEL          = 255
STATUS_LED_PPS_LEVEL             = 224

# Helpers ------------------------------------------------------------------------------------------


class LedTick(LiteXModule):
    def __init__(self, sys_clk_freq, period_s=STATUS_LED_TICK_PERIOD_S):
        self.tick = tick = Signal()

        # # #

        cycles = max(1, int(sys_clk_freq * period_s))
        count  = Signal(max=cycles)

        self.comb += tick.eq(count == (cycles - 1))
        self.sync += If(tick,
            count.eq(0)
        ).Else(
            count.eq(count + 1)
        )


class LedBreather(LiteXModule):
    def __init__(self, width=STATUS_LED_BREATH_BITS, reset=STATUS_LED_BREATH_RESET):
        self.tick  = tick  = Signal()
        self.level = level = Signal(width, reset=reset)

        # # #

        # Free-running triangle wave used as the base "breathing" envelope.
        direction = Signal(reset=1)

        self.sync += If(tick,
            If(direction,
                If(level == (2**width - 1),
                    direction.eq(0),
                    level.eq(level - 1)
                ).Else(
                    level.eq(level + 1)
                )
            ).Else(
                If(level == 0,
                    direction.eq(1),
                    level.eq(level + 1)
                ).Else(
                    level.eq(level - 1)
                )
            )
        )


class LedDoublePulse(LiteXModule):
    def __init__(self,
        period = STATUS_LED_HEARTBEAT_PERIOD,
        pulse0 = STATUS_LED_HEARTBEAT_PULSE0,
        pulse1 = STATUS_LED_HEARTBEAT_PULSE1):
        self.tick   = tick   = Signal()
        self.active = active = Signal()

        # # #

        # Generates a periodic double-pulse envelope for "boot/discovery" states.
        step = Signal(max=period)

        self.comb += active.eq(
            ((step >= pulse0[0]) & (step < pulse0[1])) |
            ((step >= pulse1[0]) & (step < pulse1[1]))
        )
        self.sync += If(tick,
            If(step == (period - 1),
                step.eq(0)
            ).Else(
                step.eq(step + 1)
            )
        )


class LedEventHold(LiteXModule):
    def __init__(self, hold_ticks):
        self.tick    = tick    = Signal()
        self.trigger = trigger = Signal()
        self.active  = active  = Signal()

        # # #

        # Stretch short activity pulses so they remain human-visible on the LED.
        pending = Signal()
        timer   = Signal(max=hold_ticks + 1)

        self.comb += active.eq(timer != 0)
        self.sync += [
            If(trigger,
                pending.eq(1)
            ),
            If(tick,
                If(pending,
                    timer.eq(hold_ticks),
                    pending.eq(0)
                ).Elif(timer != 0,
                    timer.eq(timer - 1)
                )
            )
        ]


class LedActivityBlink(LiteXModule):
    def __init__(self, period_ticks=STATUS_LED_ACTIVITY_PERIOD_TICKS, width_ticks=STATUS_LED_ACTIVITY_WIDTH_TICKS):
        assert period_ticks >= 1
        assert width_ticks  >= 1
        assert width_ticks  <= period_ticks

        self.tick    = tick    = Signal()
        self.trigger = trigger = Signal()
        self.active  = active  = Signal()

        # # #

        # Turns beat-level traffic into periodic flashes. This avoids a continuously
        # streaming datapath pinning the LED at the activity brightness forever.
        pending = Signal()
        phase   = Signal(max=period_ticks)
        timer   = Signal(max=width_ticks + 1)

        self.comb += active.eq(timer != 0)
        self.sync += [
            If(trigger,
                pending.eq(1)
            ),
            If(tick,
                If(timer != 0,
                    timer.eq(timer - 1)
                ),
                If(pending,
                    If(phase == 0,
                        timer.eq(width_ticks)
                    ),
                    If(phase == (period_ticks - 1),
                        phase.eq(0)
                    ).Else(
                        phase.eq(phase + 1)
                    ),
                    pending.eq(0)
                ).Else(
                    phase.eq(0)
                )
            )
        ]


class LedPwm(LiteXModule):
    def __init__(self, width=8):
        self.level  = level  = Signal(width)
        self.output = output = Signal()

        # # #

        # Simple duty-cycle PWM. The level input is interpreted as brightness.
        count = Signal(width)

        self.sync += count.eq(count + 1)
        self.comb += output.eq(count < level)

# Status Led ---------------------------------------------------------------------------------------


class StatusLed(LiteXModule):
    def __init__(self, sys_clk_freq):
        # IOs.
        self.time_running = time_running = Signal() # Time generator enabled/running.
        self.time_valid   = time_valid   = Signal() # Time domain has produced a non-zero time.

        self.pcie_present = pcie_present = Signal() # PCIe feature compiled in.
        self.pcie_link_up = pcie_link_up = Signal() # PCIe link is trained.
        self.dma_synced   = dma_synced   = Signal() # PCIe DMA synchronizer aligned to PPS.

        self.eth_present  = eth_present  = Signal() # Ethernet feature compiled in.
        self.eth_link_up  = eth_link_up  = Signal() # Ethernet link is trained.

        self.tx_activity     = tx_activity     = Signal() # RF transmit activity pulse.
        self.rx_activity     = rx_activity     = Signal() # RF receive activity pulse.
        self.eth_tx_activity = eth_tx_activity = Signal() # Ethernet transmit activity pulse.
        self.eth_rx_activity = eth_rx_activity = Signal() # Ethernet receive activity pulse.
        self.pps_pulse       = pps_pulse       = Signal() # PPS accent pulse.

        self.output = output = Signal()
        self.level  = level  = Signal(8)

        # # #

        # Synchronizers.
        # --------------
        pcie_link_up_sys = Signal()
        eth_link_up_sys  = Signal()
        self.specials += [
            MultiReg(pcie_link_up, pcie_link_up_sys),
            MultiReg(eth_link_up,  eth_link_up_sys),
        ]

        # Helpers.
        # --------
        self.tick = tick = LedTick(sys_clk_freq=sys_clk_freq)
        self.breather   = breather   = LedBreather()
        self.heartbeat  = heartbeat  = LedDoublePulse()
        self.tx_blink   = tx_blink   = LedActivityBlink()
        self.rx_blink   = rx_blink   = LedActivityBlink()
        self.pps_hold   = pps_hold   = LedEventHold(hold_ticks=STATUS_LED_PPS_HOLD_TICKS)
        self.pwm        = pwm        = LedPwm()

        # State Decode.
        # -------------
        booting        = Signal()
        transport_present = Signal()
        pcie_ready        = Signal()
        eth_ready         = Signal()
        transport_ready   = Signal()
        waiting_transport = Signal()
        base_level        = Signal(8)
        pps_level         = Signal(8)
        activity_level    = Signal(8)
        level0            = Signal(8)

        self.comb += [
            # All animation/effect helpers share the same low-rate timing base.
            breather.tick.eq( tick.tick),
            heartbeat.tick.eq(tick.tick),
            tx_blink.tick.eq( tick.tick),
            rx_blink.tick.eq( tick.tick),
            pps_hold.tick.eq( tick.tick),

            tx_blink.trigger.eq(tx_activity | eth_tx_activity),
            rx_blink.trigger.eq(rx_activity | eth_rx_activity),
            pps_hold.trigger.eq(pps_pulse),

            transport_present.eq(pcie_present | eth_present),
            pcie_ready.eq(     pcie_present & pcie_link_up_sys & dma_synced),
            eth_ready.eq(      eth_present  & eth_link_up_sys),
            transport_ready.eq(pcie_ready | eth_ready),

            booting.eq(          ~time_running | ~time_valid),
            waiting_transport.eq(transport_present & ~transport_ready),
        ]

        # Intensity Selection.
        # --------------------
        # Priority is:
        # 1) Base state animation (boot / transport-wait / idle)
        # 2) PPS accent
        # 3) TX/RX activity accent
        #
        # Using max() composition keeps each concern independent and easy to retune.
        self.comb += [
            base_level.eq(0),
            If(booting,
                base_level.eq(Mux(heartbeat.active, STATUS_LED_BOOT_LEVEL, 0)),
            ).Elif(waiting_transport,
                base_level.eq(Mux(heartbeat.active, STATUS_LED_DISCOVERY_LEVEL, 0)),
            ).Else(
                # Low-amplitude breathing in the normal "ready" state.
                base_level.eq(STATUS_LED_IDLE_BASE_LEVEL + (breather.level >> 1)),
            ),

            pps_level.eq(Mux(pps_hold.active, STATUS_LED_PPS_LEVEL, 0)),
            If(tx_blink.active & rx_blink.active,
                activity_level.eq(STATUS_LED_DUPLEX_LEVEL),
            ).Elif(tx_blink.active | rx_blink.active,
                activity_level.eq(STATUS_LED_ACTIVITY_LEVEL),
            ).Else(
                activity_level.eq(0),
            ),

            level0.eq(Mux(base_level > pps_level,      base_level,     pps_level)),
            level.eq( Mux(level0     > activity_level, level0,         activity_level)),

            pwm.level.eq(level),
            output.eq(  pwm.output),
        ]
