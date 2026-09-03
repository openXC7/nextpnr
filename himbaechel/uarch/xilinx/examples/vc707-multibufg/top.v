// Multiple-BUFG coverage for xc7vx485t (VC707).
//
// vc707-johnson binds exactly one BUFGCTRL, and it lands in a
// CLK_BUFG_TOP_R tile.  The phantom-BUFGCTRL guard in fasm.cc keys on
// (tile, site_y) and behaves differently for the two halves of the column
// -- BOT holds BUFGCTRL_X0Y0..X0Y15, TOP holds X0Y16..X0Y31 -- so a design
// with several bound BUFGs is the one that tells whether the guard admits
// the real sites and still suppresses the phantoms.
module top (
    input  wire sysclk_p,
    input  wire sysclk_n,
    input  wire rst,
    output wire [2:0] led
);
    wire clk_raw, rst_buf;
    wire clk0, clk1, clk2;

    IBUFDS #(.DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"), .IOSTANDARD("LVDS"))
        ibufds (.I(sysclk_p), .IB(sysclk_n), .O(clk_raw));
    IBUF ibuf_rst (.I(rst), .O(rst_buf));

    BUFG bufg0 (.I(clk_raw), .O(clk0));
    BUFG bufg1 (.I(clk_raw), .O(clk1));
    BUFG bufg2 (.I(clk_raw), .O(clk2));

    reg [7:0] c0 = 8'h00, c1 = 8'h00, c2 = 8'h00;
    always @(posedge clk0) if (rst_buf) c0 <= 8'h00; else c0 <= c0 + 8'h01;
    always @(posedge clk1) if (rst_buf) c1 <= 8'h00; else c1 <= c1 + 8'h03;
    always @(posedge clk2) if (rst_buf) c2 <= 8'h00; else c2 <= c2 + 8'h05;

    OBUF obuf0 (.I(c0[7]), .O(led[0]));
    OBUF obuf1 (.I(c1[7]), .O(led[1]));
    OBUF obuf2 (.I(c2[7]), .O(led[2]));
endmodule
