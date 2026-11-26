// Yosys techmap for the openXC7 flow: IDDR in SAME_EDGE_PIPELINED mode.
//
// nextpnr-xilinx writes the SAME_EDGE and OPPOSITE_EDGE modes of the IDDR only. In SAME_EDGE mode
// Q1 has the sample of the rising edge and Q2 the one of the falling edge before it; in
// SAME_EDGE_PIPELINED mode both come out together, one clock later: Q2 is the same, and Q1 the
// one of SAME_EDGE through one more register. An IDDR in another mode is left as it is.

module IDDR (output Q1, Q2, input C, CE, D, R, S);
    parameter DDR_CLK_EDGE = "OPPOSITE_EDGE";
    parameter INIT_Q1 = 1'b0;
    parameter INIT_Q2 = 1'b0;
    parameter [0:0] IS_C_INVERTED = 1'b0;
    parameter [0:0] IS_D_INVERTED = 1'b0;
    parameter SRTYPE = "SYNC";

    generate
        if (DDR_CLK_EDGE == "SAME_EDGE_PIPELINED") begin
            wire q1;
            IDDR #(
                .DDR_CLK_EDGE("SAME_EDGE"), .INIT_Q1(INIT_Q1), .INIT_Q2(INIT_Q2),
                .IS_C_INVERTED(IS_C_INVERTED), .IS_D_INVERTED(IS_D_INVERTED), .SRTYPE(SRTYPE)
            ) iddr (.Q1(q1), .Q2(Q2), .C(C), .CE(CE), .D(D), .R(R), .S(S));
            FDRE #(.INIT(INIT_Q1), .IS_C_INVERTED(IS_C_INVERTED)) q1_reg (
                .C(C), .CE(CE), .D(q1), .R(R), .Q(Q1));
        end else begin
            wire _TECHMAP_FAIL_ = 1'b1;
        end
    endgenerate
endmodule
