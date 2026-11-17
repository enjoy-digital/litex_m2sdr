// LiteX-M2SDR DFX demo RM: swap channel A and B (RX1<->RX2, TX1<->TX2).
//
// Samples here are in host format, before the AD9361 bit-mode conversion:
//   sc16: each 64-bit group is {Q_B, I_B, Q_A, I_A}, 16 bits each.
//   sc8:  each 32-bit group is {Q_B, I_B, Q_A, I_A},  8 bits each.
// The RM cannot see the bit-mode register, so control[2] states the format.
// BFP8 blocks carry headers and are not supported (leave the swaps off).
//
// control[0]: swap RX channels   control[1]: swap TX channels
// control[2]: 0 = sc16, 1 = sc8
//
// SPDX-License-Identifier: BSD-2-Clause

`default_nettype none

module m2sdr_dfx_datapath #(
    parameter DATA_WIDTH = 64
) (
    input  wire                  clk,
    input  wire                  rst,
    input  wire [31:0]           control,

    input  wire                  tx_sink_valid,
    output wire                  tx_sink_ready,
    input  wire [DATA_WIDTH-1:0] tx_sink_data,
    input  wire                  tx_sink_first,
    input  wire                  tx_sink_last,

    output wire                  tx_source_valid,
    input  wire                  tx_source_ready,
    output wire [DATA_WIDTH-1:0] tx_source_data,
    output wire                  tx_source_first,
    output wire                  tx_source_last,

    input  wire                  rx_sink_valid,
    output wire                  rx_sink_ready,
    input  wire [DATA_WIDTH-1:0] rx_sink_data,
    input  wire                  rx_sink_first,
    input  wire                  rx_sink_last,

    output wire                  rx_source_valid,
    input  wire                  rx_source_ready,
    output wire [DATA_WIDTH-1:0] rx_source_data,
    output wire                  rx_source_first,
    output wire                  rx_source_last,

    input  wire                  sb_sink_valid,
    output wire                  sb_sink_ready,
    input  wire [DATA_WIDTH-1:0] sb_sink_data,
    input  wire                  sb_sink_first,
    input  wire                  sb_sink_last,

    output wire                  sb_source_valid,
    input  wire                  sb_source_ready,
    output wire [DATA_WIDTH-1:0] sb_source_data,
    output wire                  sb_source_first,
    output wire                  sb_source_last,

    input  wire [63:0]           time_ns,
    output wire [31:0]           sb_status,

    output wire [7:0]            rm_id
);
    wire swap_rx = control[0];
    wire swap_tx = control[1];
    wire sc8     = control[2];

    // Channel B in A's place: swap the halves of every group.
    function [DATA_WIDTH-1:0] swap_channels(input [DATA_WIDTH-1:0] d, input is_sc8);
        integer g;
        begin
            for (g = 0; g < DATA_WIDTH/64; g = g + 1) begin
                if (is_sc8) begin
                    swap_channels[g*64 +:  16] = d[g*64 + 16 +: 16];
                    swap_channels[g*64 + 16 +: 16] = d[g*64 +:  16];
                    swap_channels[g*64 + 32 +: 16] = d[g*64 + 48 +: 16];
                    swap_channels[g*64 + 48 +: 16] = d[g*64 + 32 +: 16];
                end else begin
                    swap_channels[g*64 +:  32] = d[g*64 + 32 +: 32];
                    swap_channels[g*64 + 32 +: 32] = d[g*64 +:  32];
                end
            end
        end
    endfunction

    // Pure data permutation: handshakes and framing pass straight through.
    assign tx_source_valid = tx_sink_valid;
    assign tx_sink_ready   = tx_source_ready;
    assign tx_source_data  = swap_tx ? swap_channels(tx_sink_data, sc8) : tx_sink_data;
    assign tx_source_first = tx_sink_first;
    assign tx_source_last  = tx_sink_last;

    assign rx_source_valid = rx_sink_valid;
    assign rx_sink_ready   = rx_source_ready;
    assign rx_source_data  = swap_rx ? swap_channels(rx_sink_data, sc8) : rx_sink_data;
    assign rx_source_first = rx_sink_first;
    assign rx_source_last  = rx_sink_last;

    // Side band (DMA1) unused: nothing to the host, host words are accepted and dropped.
    assign sb_sink_ready   = 1'b1;
    assign sb_source_valid = 1'b0;
    assign sb_source_data  = {DATA_WIDTH{1'b0}};
    assign sb_source_first = 1'b0;
    assign sb_source_last  = 1'b0;
    assign sb_status       = 32'd0;

    assign rm_id = 8'd2;
endmodule

`default_nettype wire
