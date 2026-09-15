// MMCM with several outputs, one of which leaves CLKOUT*_PHASE at its
// default.
//
// The phase default is the point of this design.  UG472 gives
// CLKOUT*_PHASE a default of 0 degrees, and write_pll in fasm.cc uses 0 --
// but write_mmcm_clkout defaulted to 1, so any MMCM output whose phase was
// not written explicitly got a phase shift nobody asked for.  Nothing in
// the gate noticed, because every MMCM in every demo project sets all of
// its phases explicitly.
//
// CLKOUT0 sets PHASE explicitly, CLKOUT1 leaves it at the default, and
// CLKOUT2 sets a non-zero phase.  A change to the default therefore moves
// this design's FASM and nothing else's.
module top (
    input  wire sysclk_p,
    input  wire sysclk_n,
    input  wire rst,
    output wire [2:0] led
);
    wire clk_raw, clk_in, rst_buf;
    IBUFDS #(.DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"), .IOSTANDARD("LVDS"))
        ibufds (.I(sysclk_p), .IB(sysclk_n), .O(clk_raw));
    BUFG bufg_in (.I(clk_raw), .O(clk_in));
    IBUF ibuf_rst (.I(rst), .O(rst_buf));

    wire clkout0, clkout1, clkout2, clkfb, locked;
    wire clk0, clk1, clk2;

    MMCME2_BASE #(
        .CLKIN1_PERIOD    (5.000),   // 200 MHz VC707 system clock
        .DIVCLK_DIVIDE    (1),
        .CLKFBOUT_MULT_F  (5.000),   // 1 GHz VCO
        .CLKOUT0_DIVIDE_F (10.000),  // 100 MHz
        .CLKOUT0_PHASE    (0.000),   // explicit zero
        .CLKOUT1_DIVIDE   (20),      // 50 MHz, PHASE left at the default
        .CLKOUT2_DIVIDE   (8),       // 125 MHz
        .CLKOUT2_PHASE    (45.000),  // explicit non-zero
        .BANDWIDTH        ("OPTIMIZED")
    ) mmcm (
        .CLKIN1   (clk_in),
        .CLKFBIN  (clkfb),
        .CLKFBOUT (clkfb),
        .CLKOUT0  (clkout0),
        .CLKOUT1  (clkout1),
        .CLKOUT2  (clkout2),
        .LOCKED   (locked),
        .PWRDWN   (1'b0),
        .RST      (rst_buf)
    );

    BUFG bufg0 (.I(clkout0), .O(clk0));
    BUFG bufg1 (.I(clkout1), .O(clk1));
    BUFG bufg2 (.I(clkout2), .O(clk2));

    // one counter per output, so each clock reaches real logic
    reg [23:0] c0, c1, c2;
    always @(posedge clk0) c0 <= rst_buf ? 24'd0 : c0 + 1'b1;
    always @(posedge clk1) c1 <= rst_buf ? 24'd0 : c1 + 1'b1;
    always @(posedge clk2) c2 <= rst_buf ? 24'd0 : c2 + 1'b1;

    assign led[0] = c0[23] & locked;
    assign led[1] = c1[23];
    assign led[2] = c2[23];
endmodule
