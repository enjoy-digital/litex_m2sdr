// Testbench of the side-band detector in m2sdr_dfx_datapath_loopback.v (run by test/test_dfx.py).
// Prints "PASS" on success, fails with $fatal otherwise.
`timescale 1ns/1ps

module tb;
    parameter DATA_WIDTH = 64;
    localparam WORDS = 256/DATA_WIDTH;
    localparam TIME_STEP = 1024;                     // ns per cycle: a level record every 1024 cycles.
    localparam BURST_START = 4000, BURST_LEN = 600;  // In RX words.

    reg clk = 0, rst = 1;
    always #1 clk = ~clk;

    reg  [63:0] time_ns = 0;
    reg  [31:0] control = {16'd1000, 8'd0, 4'd4, 4'b0010};  // threshold 1000, k = 4, detector on.
    reg         sb_ready = 1;
    reg  [DATA_WIDTH-1:0] rx_data = 0;
    integer     cycle = 0;

    wire rx_sink_ready, sb_valid, sb_first, sb_last;
    wire [DATA_WIDTH-1:0] sb_data;
    wire [31:0] sb_status;
    wire [7:0]  rm_id;
    wire tx_sink_ready, tx_source_valid, tx_source_first, tx_source_last;
    wire rx_source_valid, rx_source_first, rx_source_last, sb_sink_ready;
    wire [DATA_WIDTH-1:0] tx_source_data, rx_source_data;

    m2sdr_dfx_datapath #(.DATA_WIDTH(DATA_WIDTH)) dut (
        .clk(clk), .rst(rst), .control(control),
        .tx_sink_valid(1'b0), .tx_sink_ready(tx_sink_ready), .tx_sink_data({DATA_WIDTH{1'b0}}),
        .tx_sink_first(1'b0), .tx_sink_last(1'b0),
        .tx_source_valid(tx_source_valid), .tx_source_ready(1'b1), .tx_source_data(tx_source_data),
        .tx_source_first(tx_source_first), .tx_source_last(tx_source_last),
        .rx_sink_valid(1'b1), .rx_sink_ready(rx_sink_ready), .rx_sink_data(rx_data),
        .rx_sink_first(1'b0), .rx_sink_last(1'b0),
        .rx_source_valid(rx_source_valid), .rx_source_ready(1'b1), .rx_source_data(rx_source_data),
        .rx_source_first(rx_source_first), .rx_source_last(rx_source_last),
        .sb_sink_valid(1'b0), .sb_sink_ready(sb_sink_ready), .sb_sink_data({DATA_WIDTH{1'b0}}),
        .sb_sink_first(1'b0), .sb_sink_last(1'b0),
        .sb_source_valid(sb_valid), .sb_source_ready(sb_ready), .sb_source_data(sb_data),
        .sb_source_first(sb_first), .sb_source_last(sb_last),
        .time_ns(time_ns), .sb_status(sb_status), .rm_id(rm_id)
    );

    // Channel A sample: small pseudo-random noise, a strong carrier during the burst.
    reg [31:0] lfsr = 32'h1;
    always @(posedge clk) lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
    wire burst = (cycle >= BURST_START) && (cycle < BURST_START + BURST_LEN);
    wire signed [15:0] i_s = burst ? 16'sd3000 : $signed({{11{lfsr[4]}}, lfsr[4:0]});
    wire signed [15:0] q_s = burst ? -16'sd200 : $signed({{11{lfsr[9]}}, lfsr[9:5]});

    // Collected records.
    reg [255:0] rec;
    integer word = 0, det = 0, sta = 0, last_seq = -1;
    reg [63:0] det_start, det_end; reg [31:0] det_peak, det_words;

    always @(posedge clk) begin
        cycle   <= cycle + 1;
        time_ns <= time_ns + TIME_STEP;
        rx_data <= {{(DATA_WIDTH-32){1'b0}}, q_s, i_s};
        if (cycle == 4) rst <= 0;
        if (!rx_sink_ready) $fatal(1, "RX path stalled at cycle %0d", cycle);
        if (sb_valid && sb_ready) begin
            if ((word == 0) != sb_first) $fatal(1, "first flag wrong at word %0d", word);
            rec[word*DATA_WIDTH +: DATA_WIDTH] = sb_data;
            if (word == WORDS-1) begin
                if (!sb_last) $fatal(1, "last flag missing");
                if ($signed(rec[223:192]) <= last_seq) $fatal(1, "sequence not increasing");
                last_seq = rec[223:192];
                if (rec[255:224] == 32'h44455431) begin
                    det = det + 1;
                    det_start = rec[63:0]; det_end = rec[127:64];
                    det_peak = rec[191:160]; det_words = rec[159:128];
                end else if (rec[255:224] == 32'h53544131) sta = sta + 1;
                else $fatal(1, "unknown record tag %h", rec[255:224]);
                word = 0;
            end else word = word + 1;
        end
        // Phase 2: the host stops reading; level records must be dropped and counted.
        if (cycle == 9000) sb_ready <= 0;
        if (cycle == 13000) begin
            if (det != 1) $fatal(1, "expected 1 DET1 record, got %0d", det);
            if (det_start < BURST_START*TIME_STEP || det_start > (BURST_START+40)*TIME_STEP)
                $fatal(1, "burst start %0d ns not near %0d ns", det_start, BURST_START*TIME_STEP);
            if (det_end < (BURST_START+BURST_LEN)*TIME_STEP || det_end > (BURST_START+BURST_LEN+80)*TIME_STEP)
                $fatal(1, "burst end %0d ns not near %0d ns", det_end, (BURST_START+BURST_LEN)*TIME_STEP);
            if (det_peak < 2900 || det_peak > 3300) $fatal(1, "peak level %0d, expected about 3200", det_peak);
            if (det_words < BURST_LEN-20 || det_words > BURST_LEN+80) $fatal(1, "burst length %0d words", det_words);
            if (sta < 7) $fatal(1, "only %0d level records", sta);
            if (sb_status[31:16] < 2) $fatal(1, "drops not counted: status %h", sb_status);
            if (sb_status[15:0] != det + sta) $fatal(1, "sent count %0d != %0d records", sb_status[15:0], det + sta);
            $display("PASS width=%0d det_start=%0d det_end=%0d peak=%0d words=%0d sta=%0d dropped=%0d",
                DATA_WIDTH, det_start, det_end, det_peak, det_words, sta, sb_status[31:16]);
            $finish;
        end
    end
endmodule
