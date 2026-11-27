// Yosys techmap for the openXC7 flow: IDDR in SAME_EDGE_PIPELINED mode, in the fabric.
//
// With nextpnr-xilinx an input registered by an IDDR in the IO tile does not work on the board
// (SI5351 I2C, AD9361 RX data), with the same configuration of the pad and of the ILOGIC as in a
// Vivado bitstream. The capture is done with flip-flops of the fabric instead: one on each edge
// of the clock, then both on the rising edge, which is what SAME_EDGE_PIPELINED gives (Q1 the
// sample of a rising edge, Q2 the one of the falling edge after it, together one clock later).
// The timing is not the one of the IO tile: the delay from the pad to the flip-flops is routing.
// An IDDR in another mode is left as it is.

module IDDR (output Q1, Q2, input C, CE, D, R, S);
    parameter DDR_CLK_EDGE = "OPPOSITE_EDGE";
    parameter INIT_Q1 = 1'b0;
    parameter INIT_Q2 = 1'b0;
    parameter [0:0] IS_C_INVERTED = 1'b0;
    parameter [0:0] IS_D_INVERTED = 1'b0;
    parameter SRTYPE = "SYNC";

    generate
        if (DDR_CLK_EDGE == "SAME_EDGE_PIPELINED" && IS_D_INVERTED == 1'b0) begin
            wire rise, fall;
            FDRE #(.INIT(1'b0), .IS_C_INVERTED( IS_C_INVERTED)) rise_reg (.C(C), .CE(CE), .D(D), .R(R), .Q(rise));
            FDRE #(.INIT(1'b0), .IS_C_INVERTED(!IS_C_INVERTED)) fall_reg (.C(C), .CE(CE), .D(D), .R(R), .Q(fall));
            FDRE #(.INIT(INIT_Q1), .IS_C_INVERTED(IS_C_INVERTED)) q1_reg (.C(C), .CE(CE), .D(rise), .R(R), .Q(Q1));
            FDRE #(.INIT(INIT_Q2), .IS_C_INVERTED(IS_C_INVERTED)) q2_reg (.C(C), .CE(CE), .D(fall), .R(R), .Q(Q2));
        end else begin
            wire _TECHMAP_FAIL_ = 1'b1;
        end
    endgenerate
endmodule
