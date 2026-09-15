// HP-bank differential I/O coverage for xc7vx485t (VC707).
//
// vc707-johnson covers the differential RECEIVER only (input-only LVDS
// clock).  This adds the two HP differential shapes it does not reach:
// an OBUFDS driver, and an IOBUFDS whose pad is both driven and received.
// The bidirectional DIFF_SSTL15 pad is the shape that made fasm2frames
// reject litex-ddr-qmtech-kintex7 before the receiver slew line was
// guarded on !is_output -- nothing on xc7v exercised it.
module top (
    input  wire sysclk_p,
    input  wire sysclk_n,
    input  wire rst,
    output wire dout_p,
    output wire dout_n,
    inout  wire dq_p,
    inout  wire dq_n,
    output wire [1:0] led
);
    wire clk_raw, clk, rst_buf, dq_in;

    IBUFDS #(.DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"), .IOSTANDARD("LVDS"))
        ibufds (.I(sysclk_p), .IB(sysclk_n), .O(clk_raw));
    BUFG bufg (.I(clk_raw), .O(clk));
    IBUF ibuf_rst (.I(rst), .O(rst_buf));

    reg [7:0] cnt = 8'h00;
    always @(posedge clk)
        if (rst_buf)
            cnt <= 8'h00;
        else
            cnt <= cnt + 8'h01;

    OBUFDS #(.IOSTANDARD("LVDS")) obufds (.I(cnt[7]), .O(dout_p), .OB(dout_n));

    IOBUFDS #(.IOSTANDARD("DIFF_SSTL15"))
        iobufds (.I(cnt[6]), .T(cnt[5]), .O(dq_in), .IO(dq_p), .IOB(dq_n));

    OBUF obuf0 (.I(dq_in), .O(led[0]));
    OBUF obuf1 (.I(cnt[4]), .O(led[1]));
endmodule
