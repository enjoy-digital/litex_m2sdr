// SPDX-License-Identifier: BSD-2-Clause
// Example alternate RFIC-plane RM: control[0] loops host TX samples back to host RX at the RFIC
// plane (the AD9361 receives nothing, its RX samples are drained). rm_id 1.

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
    wire enable = control[0];

    assign tx_sink_ready   = enable ? rx_source_ready : tx_source_ready;
    assign tx_source_valid = enable ? 1'b0 : tx_sink_valid;
    assign tx_source_data  = tx_sink_data;
    assign tx_source_first = tx_sink_first;
    assign tx_source_last  = tx_sink_last;

    assign rx_sink_ready   = enable ? 1'b1 : rx_source_ready;
    assign rx_source_valid = enable ? tx_sink_valid : rx_sink_valid;
    assign rx_source_data  = enable ? tx_sink_data  : rx_sink_data;
    assign rx_source_first = enable ? tx_sink_first : rx_sink_first;
    assign rx_source_last  = enable ? tx_sink_last  : rx_sink_last;

    assign rm_id = 8'd1;
endmodule

`default_nettype wire
