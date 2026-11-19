//
// This file is part of LiteX-M2SDR.
//
// SPDX-License-Identifier: BSD-2-Clause
//
// Minimal BRAM_TDP_MACRO replacement (inferred block RAM).
//
// Vivado resolves BRAM_TDP_MACRO from its UNIMACRO library; the open-source flow (openXC7) has no
// UNIMACRO, and the Vivado-generated pcie_s7 IP sources instantiate it for their TX/RX buffers.
// Only used with --toolchain=openxc7.
//
// Limited to what the pcie_s7 IP uses: 36Kb RAM, same width on both ports, port A write-only
// (DOA is not driven from the RAM) and port B read-only (DIB/WEB ignored).

module BRAM_TDP_MACRO #(
    parameter         DEVICE        = "7SERIES",
    parameter         BRAM_SIZE     = "36Kb",
    parameter integer DOA_REG       = 0,
    parameter integer DOB_REG       = 0,
    parameter integer READ_WIDTH_A  = 36,
    parameter integer READ_WIDTH_B  = 36,
    parameter integer WRITE_WIDTH_A = 36,
    parameter integer WRITE_WIDTH_B = 36,
    parameter         WRITE_MODE_A  = "WRITE_FIRST",
    parameter         WRITE_MODE_B  = "WRITE_FIRST",
    // Derived (do not override).
    parameter integer WIDTH         = WRITE_WIDTH_A,
    parameter integer ADDR_WIDTH    = (WIDTH <=  1) ? 15 :
                                      (WIDTH <=  2) ? 14 :
                                      (WIDTH <=  4) ? 13 :
                                      (WIDTH <=  9) ? 12 :
                                      (WIDTH <= 18) ? 11 : 10,
    parameter integer WE_WIDTH      = (WIDTH <=  9) ? 1 :
                                      (WIDTH <= 18) ? 2 : 4
) (
    output wire [WIDTH-1:0]      DOA,
    output wire [WIDTH-1:0]      DOB,
    input  wire [ADDR_WIDTH-1:0] ADDRA,
    input  wire [ADDR_WIDTH-1:0] ADDRB,
    input  wire                  CLKA,
    input  wire                  CLKB,
    input  wire [WIDTH-1:0]      DIA,
    input  wire [WIDTH-1:0]      DIB,
    input  wire                  ENA,
    input  wire                  ENB,
    input  wire                  REGCEA,
    input  wire                  REGCEB,
    input  wire                  RSTA,
    input  wire                  RSTB,
    input  wire [WE_WIDTH-1:0]   WEA,
    input  wire [WE_WIDTH-1:0]   WEB
);

    localparam integer BYTE_WIDTH = WIDTH/WE_WIDTH;

    reg [WIDTH-1:0] mem [0:(1 << ADDR_WIDTH)-1];

    // Port A: write.
    integer i;
    always @(posedge CLKA) begin
        if (ENA) begin
            for (i = 0; i < WE_WIDTH; i = i + 1) begin
                if (WEA[i])
                    mem[ADDRA][i*BYTE_WIDTH +: BYTE_WIDTH] <= DIA[i*BYTE_WIDTH +: BYTE_WIDTH];
            end
        end
    end
    assign DOA = {WIDTH{1'b0}};

    // Port B: read (output latch + optional output register).
    reg [WIDTH-1:0] dob_latch = {WIDTH{1'b0}};
    reg [WIDTH-1:0] dob_reg   = {WIDTH{1'b0}};
    always @(posedge CLKB) begin
        if (ENB) begin
            if (RSTB)
                dob_latch <= {WIDTH{1'b0}};
            else
                dob_latch <= mem[ADDRB];
        end
    end
    always @(posedge CLKB) begin
        if (RSTB)
            dob_reg <= {WIDTH{1'b0}};
        else if (REGCEB)
            dob_reg <= dob_latch;
    end
    assign DOB = DOB_REG ? dob_reg : dob_latch;

endmodule
