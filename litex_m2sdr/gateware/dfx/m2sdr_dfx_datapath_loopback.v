// SPDX-License-Identifier: BSD-2-Clause
// Example alternate RFIC-plane RM (rm_id 1):
//
// - control[0] loops host TX samples back to host RX at the RFIC plane (the AD9361 receives
//   nothing, its RX samples are drained).
// - Side-band demo (DMA1): a burst detector on channel A of the AD9361 RX samples (sc16,
//   {Q_A, I_A} in the low 32 bits of each word). The level is the mean of |I| + |Q| over about
//   2^k words. A burst starts when the level exceeds the threshold and ends when it drops to half
//   of it. control[1] enables it, control[7:4] = k, control[31:16] = threshold.
//
//   Records are 32 bytes (4 words at 64 bits, 2 at 128), little endian, time in ns:
//     DET1: start_time, end_time, {peak_level, burst_words}, {"DET1", sequence}   (one per burst)
//     STA1: time, 0, {level, rx_words}, {"STA1", sequence}   (about every 1.05 ms, so DMA
//           buffers keep filling and detections reach the host promptly)
//   The RX path is never stalled: a record that finds the side band busy is dropped.
//   sb_status = {dropped[15:0], sent[15:0]}.

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

    // Side-band demo: burst detector + periodic level records ------------------------------------
    localparam       WORDS = 256/DATA_WIDTH;        // Words per 32-byte record.
    localparam [1:0] LAST  = WORDS - 1;
    wire        det_enable = control[1];
    wire [3:0]  k          = control[7:4];
    wire [16:0] threshold  = {1'b0, control[31:16]};

    // Level: exponential average of |I| + |Q| on channel A (acc = 2^k x mean).
    wire        rx_fire = rx_sink_valid & rx_sink_ready;
    wire signed [15:0] i_a = rx_sink_data[15:0];
    wire signed [15:0] q_a = rx_sink_data[31:16];
    wire [16:0] mag = (i_a[15] ? -{i_a[15], i_a} : {1'b0, i_a}) + (q_a[15] ? -{q_a[15], q_a} : {1'b0, q_a});
    // The inputs are registered once: the path from the partition boundary then ends here, and
    // |I| + |Q| and the accumulator start from a register (the 125 MHz build missed timing on it).
    reg         det_fire = 1'b0;
    reg  [16:0] det_mag  = 17'd0;
    reg  [32:0] acc   = 33'd0;
    wire [32:0] level_full = acc >> k;              // acc stays below 2^k x 2^17.
    wire [16:0] level = level_full[16:0];
    reg  [31:0] rx_words = 32'd0;

    reg         in_burst = 1'b0;
    reg  [63:0] burst_start = 64'd0;
    reg  [16:0] burst_peak  = 17'd0;
    reg  [31:0] burst_words = 32'd0;
    reg         tick_last   = 1'b0;

    // One pending record, serialized into WORDS side-band words.
    reg  [255:0] record   = 256'd0;
    reg          pending  = 1'b0;
    reg  [1:0]   word_idx = 2'd0;
    reg  [31:0]  seq_num = 32'd0;
    reg  [15:0]  sent     = 16'd0;
    reg  [15:0]  dropped  = 16'd0;

    wire tick      = time_ns[20] != tick_last;      // About every 1.05 ms.
    wire burst_end = in_burst & det_fire & (level <= (threshold >> 1));
    wire out_fire  = sb_source_valid & sb_source_ready;

    always @(posedge clk) begin
        if (rst | ~det_enable) begin
            det_fire    <= 1'b0;
            acc         <= 33'd0;
            rx_words    <= 32'd0;
            in_burst    <= 1'b0;
            pending     <= 1'b0;
            word_idx    <= 2'd0;
            tick_last   <= time_ns[20];
            if (rst) begin
                seq_num <= 32'd0;
                sent     <= 16'd0;
                dropped  <= 16'd0;
            end
        end else begin
            tick_last <= time_ns[20];
            det_fire  <= rx_fire;
            det_mag   <= mag;
            if (det_fire) begin
                acc      <= acc - (acc >> k) + {16'd0, det_mag};
                rx_words <= rx_words + 32'd1;
            end

            // Burst tracking (hysteresis: start above threshold, end at threshold / 2).
            if (det_fire & ~in_burst & (level > threshold)) begin
                in_burst    <= 1'b1;
                burst_start <= time_ns;
                burst_peak  <= level;
                burst_words <= 32'd0;
            end else if (det_fire & in_burst) begin
                burst_words <= burst_words + 32'd1;
                if (level > burst_peak)
                    burst_peak <= level;
                if (burst_end)
                    in_burst <= 1'b0;
            end

            // Serialize the pending record; the last word frees the slot.
            if (out_fire) begin
                word_idx <= word_idx + 2'd1;
                if (word_idx == LAST) begin
                    pending  <= 1'b0;
                    word_idx <= 2'd0;
                    sent     <= sent + 16'd1;
                end
            end

            // New record (a burst end wins over a level tick in the same cycle); drop it if the
            // slot is still busy, so the RX path never waits for the host.
            if (burst_end | tick) begin
                if (pending & ~(out_fire & (word_idx == LAST))) begin
                    dropped <= dropped + 16'd1;
                end else begin
                    pending  <= 1'b1;
                    seq_num <= seq_num + 32'd1;
                    record   <= burst_end ?
                        {32'h44455431, seq_num, {15'd0, burst_peak}, burst_words + 32'd1, time_ns, burst_start} :
                        {32'h53544131, seq_num, {15'd0, level}, rx_words, 64'd0, time_ns};
                end
            end
        end
    end

    assign sb_source_valid = pending;
    assign sb_source_data  = record[word_idx*DATA_WIDTH +: DATA_WIDTH];
    assign sb_source_first = word_idx == 2'd0;
    assign sb_source_last  = word_idx == LAST;
    assign sb_sink_ready   = 1'b1;              // Host words are not used by this RM.
    assign sb_status       = {dropped, sent};

    assign rm_id = 8'd1;
endmodule

`default_nettype wire
