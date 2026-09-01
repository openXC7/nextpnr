// ILOGIC delayed-input coverage for xc7vx485t (VC707).
//
// The RIOI pseudo-pip that selects the delayed ILOGIC input --
// IOI_ILOGIC<i>_O <- RIOI_ILOGIC<i>_DDLY, emitting ILOGIC_Y<i>.IDELMUXE3.P0
// -- has no coverage in any gate: no FASM produced by the five demo
// projects contains an IDELMUXE3 line.  The XC7V branch of that mapping
// (which drops ILOGIC_Y*.ZINV_D, absent from virtex7/segbits_rioi.db) is
// therefore asserted from the database alone.  This design routes a pad
// through IDELAYE2 into a flop so the mapping is actually exercised.
module top (
    input  wire sysclk_p,
    input  wire sysclk_n,
    input  wire rst,
    input  wire din,
    output wire [1:0] led
);
    wire clk_raw, clk, rst_buf, din_buf, din_dly;

    IBUFDS #(.DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"), .IOSTANDARD("LVDS"))
        ibufds (.I(sysclk_p), .IB(sysclk_n), .O(clk_raw));
    BUFG bufg (.I(clk_raw), .O(clk));
    IBUF ibuf_rst (.I(rst), .O(rst_buf));
    IBUF ibuf_din (.I(din), .O(din_buf));

    IDELAYE2 #(
        .IDELAY_TYPE("FIXED"),
        .IDELAY_VALUE(15),
        .DELAY_SRC("IDATAIN"),
        .HIGH_PERFORMANCE_MODE("TRUE"),
        .SIGNAL_PATTERN("DATA"),
        .REFCLK_FREQUENCY(200.0),
        .CINVCTRL_SEL("FALSE"),
        .PIPE_SEL("FALSE")
    ) idelay (
        .C(1'b0), .REGRST(1'b0), .LD(1'b0), .CE(1'b0), .INC(1'b0),
        .CINVCTRL(1'b0), .CNTVALUEIN(5'b00000), .LDPIPEEN(1'b0),
        .IDATAIN(din_buf), .DATAIN(1'b0),
        .DATAOUT(din_dly), .CNTVALUEOUT()
    );

    reg [1:0] q = 2'b00;
    always @(posedge clk)
        if (rst_buf)
            q <= 2'b00;
        else
            q <= {q[0], din_dly};

    OBUF obuf0 (.I(q[1]), .O(led[0]));
    OBUF obuf1 (.I(q[0]), .O(led[1]));
endmodule
