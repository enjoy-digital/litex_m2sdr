// SPDX-License-Identifier: BSD-2-Clause
// Default RFIC-plane RM: transparent, like BaseSoC.add_rfic_datapath(). rm_id 0.

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
    assign tx_sink_ready   = tx_source_ready;
    assign tx_source_valid = tx_sink_valid;
    assign tx_source_data  = tx_sink_data;
    assign tx_source_first = tx_sink_first;
    assign tx_source_last  = tx_sink_last;

    assign rx_sink_ready   = rx_source_ready;
    assign rx_source_valid = rx_sink_valid;
    assign rx_source_data  = rx_sink_data;
    assign rx_source_first = rx_sink_first;
    assign rx_source_last  = rx_sink_last;

    // Side band (DMA1): echo host words back, so dma-test --external-loopback on the DMA1 device
    // checks the side-band path of the default image. Nothing is sent unless the host writes.
    assign sb_sink_ready   = sb_source_ready;
    assign sb_source_valid = sb_sink_valid;
    assign sb_source_data  = sb_sink_data;
    assign sb_source_first = sb_sink_first;
    assign sb_source_last  = sb_sink_last;
    assign sb_status       = 32'd0;

    assign rm_id = 8'd0;
endmodule

`default_nettype wire
