module custom_processing_passthrough #(
    parameter integer DATA_WIDTH = 64
) (
    input  wire                  clk,
    input  wire                  rst,

    input  wire                  s_axis_tx_tvalid,
    output wire                  s_axis_tx_tready,
    input  wire [DATA_WIDTH-1:0] s_axis_tx_tdata,
    input  wire                  s_axis_tx_tlast,
    output wire                  m_axis_tx_tvalid,
    input  wire                  m_axis_tx_tready,
    output wire [DATA_WIDTH-1:0] m_axis_tx_tdata,
    output wire                  m_axis_tx_tlast,

    input  wire                  s_axis_rx_tvalid,
    output wire                  s_axis_rx_tready,
    input  wire [DATA_WIDTH-1:0] s_axis_rx_tdata,
    input  wire                  s_axis_rx_tlast,
    output wire                  m_axis_rx_tvalid,
    input  wire                  m_axis_rx_tready,
    output wire [DATA_WIDTH-1:0] m_axis_rx_tdata,
    output wire                  m_axis_rx_tlast
);

// Dummy client-processing example:
// replace the passthrough assignments below with custom AXI-Stream logic.

assign m_axis_tx_tvalid = s_axis_tx_tvalid;
assign s_axis_tx_tready = m_axis_tx_tready;
assign m_axis_tx_tdata  = s_axis_tx_tdata;
assign m_axis_tx_tlast  = s_axis_tx_tlast;

assign m_axis_rx_tvalid = s_axis_rx_tvalid;
assign s_axis_rx_tready = m_axis_rx_tready;
assign m_axis_rx_tdata  = s_axis_rx_tdata;
assign m_axis_rx_tlast  = s_axis_rx_tlast;

endmodule
