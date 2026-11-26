// Yosys techmap for the openXC7 flow: single-port LUT RAMs as dual-port ones.
//
// synth_xilinx maps a small single-port memory to RAM32X1S, RAM64X1S, RAM128X1S or RAM256X1S.
// nextpnr-xilinx packs the dual-port primitives (RAMnX1D) but not these: each is replaced by the
// dual-port primitive of the same depth (two of 128 bits for 256), read on its write port (SPO),
// the second read port left unused.

module RAM32X1S (output O, input A0, A1, A2, A3, A4, D, WCLK, WE);
    parameter [31:0] INIT = 32'h0;
    parameter [0:0] IS_WCLK_INVERTED = 1'b0;
    RAM32X1D #(.INIT(INIT), .IS_WCLK_INVERTED(IS_WCLK_INVERTED)) _TECHMAP_REPLACE_ (
        .A0(A0), .A1(A1), .A2(A2), .A3(A3), .A4(A4),
        .DPRA0(A0), .DPRA1(A1), .DPRA2(A2), .DPRA3(A3), .DPRA4(A4),
        .D(D), .WCLK(WCLK), .WE(WE), .SPO(O), .DPO());
endmodule

module RAM64X1S (output O, input A0, A1, A2, A3, A4, A5, D, WCLK, WE);
    parameter [63:0] INIT = 64'h0;
    parameter [0:0] IS_WCLK_INVERTED = 1'b0;
    RAM64X1D #(.INIT(INIT), .IS_WCLK_INVERTED(IS_WCLK_INVERTED)) _TECHMAP_REPLACE_ (
        .A0(A0), .A1(A1), .A2(A2), .A3(A3), .A4(A4), .A5(A5),
        .DPRA0(A0), .DPRA1(A1), .DPRA2(A2), .DPRA3(A3), .DPRA4(A4), .DPRA5(A5),
        .D(D), .WCLK(WCLK), .WE(WE), .SPO(O), .DPO());
endmodule

module RAM128X1S (output O, input A0, A1, A2, A3, A4, A5, A6, D, WCLK, WE);
    parameter [127:0] INIT = 128'h0;
    parameter [0:0] IS_WCLK_INVERTED = 1'b0;
    RAM128X1D #(.INIT(INIT), .IS_WCLK_INVERTED(IS_WCLK_INVERTED)) _TECHMAP_REPLACE_ (
        .A({A6, A5, A4, A3, A2, A1, A0}), .DPRA({A6, A5, A4, A3, A2, A1, A0}),
        .D(D), .WCLK(WCLK), .WE(WE), .SPO(O), .DPO());
endmodule

// 256 bits: nextpnr-xilinx has no 256-bit LUT RAM on the 7-series. Two of 128 bits, the write
// enable decoded and the output selected by A[7], in LUTs (no logic is mapped after this pass).
module RAM256X1S (output O, input [7:0] A, input D, WCLK, WE);
    parameter [255:0] INIT = 256'h0;
    parameter [0:0] IS_WCLK_INVERTED = 1'b0;
    wire we_lo, we_hi, o_lo, o_hi;
    LUT2 #(.INIT(4'b0010)) we_lo_lut (.I0(WE), .I1(A[7]), .O(we_lo));   // WE & ~A[7]
    LUT2 #(.INIT(4'b1000)) we_hi_lut (.I0(WE), .I1(A[7]), .O(we_hi));   // WE &  A[7]
    RAM128X1D #(.INIT(INIT[127:0]), .IS_WCLK_INVERTED(IS_WCLK_INVERTED)) lo (
        .A(A[6:0]), .DPRA(A[6:0]), .D(D), .WCLK(WCLK), .WE(we_lo), .SPO(o_lo), .DPO());
    RAM128X1D #(.INIT(INIT[255:128]), .IS_WCLK_INVERTED(IS_WCLK_INVERTED)) hi (
        .A(A[6:0]), .DPRA(A[6:0]), .D(D), .WCLK(WCLK), .WE(we_hi), .SPO(o_hi), .DPO());
    LUT3 #(.INIT(8'hCA)) o_lut (.I0(o_lo), .I1(o_hi), .I2(A[7]), .O(O));  // A[7] ? o_hi : o_lo
endmodule
