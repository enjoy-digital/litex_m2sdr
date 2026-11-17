// SPDX-License-Identifier: BSD-2-Clause
// Default RFIC-plane RM: transparent, like BaseSoC.add_rfic_datapath(). rm_id 0.

`default_nettype none

module m2sdr_dfx_datapath #(
    parameter DATA_WIDTH = 64
) (
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

    assign rm_id = 8'd0;
endmodule

`default_nettype wire
