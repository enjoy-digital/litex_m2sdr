//
// This file is part of LiteX-M2SDR.
//
// SPDX-License-Identifier: BSD-2-Clause
//
// Minimal xpm_cdc_single replacement (single-bit multi-flop synchronizer).
//
// Vivado resolves xpm_cdc_single from its XPM library; the open-source flow (openXC7) has no XPM,
// and the Vivado-generated pcie_s7 IP sources instantiate it. Only used with --toolchain=openxc7.

module xpm_cdc_single #(
    parameter DEST_SYNC_FF   = 4,
    parameter INIT_SYNC_FF   = 0,
    parameter SIM_ASSERT_CHK = 0,
    parameter SRC_INPUT_REG  = 1
) (
    input  wire src_clk,
    input  wire src_in,
    input  wire dest_clk,
    output wire dest_out
);

    // Optional source register.
    reg  src_ff = 1'b0;
    wire src    = SRC_INPUT_REG ? src_ff : src_in;
    always @(posedge src_clk)
        src_ff <= src_in;

    // Destination synchronizer.
    (* ASYNC_REG = "TRUE" *) reg [DEST_SYNC_FF-1:0] sync_ff = {DEST_SYNC_FF{1'b0}};
    always @(posedge dest_clk)
        sync_ff <= {sync_ff[DEST_SYNC_FF-2:0], src};

    assign dest_out = sync_ff[DEST_SYNC_FF-1];

endmodule
