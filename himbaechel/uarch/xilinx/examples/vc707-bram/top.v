// Block RAM with byte write-enables, marked for BRAM rather than left to
// the area heuristic.
//
// Covers the RAMB*E1 write path that every LiteX design depends on and no
// small design in the gate reaches: a byte-enabled write port, a
// registered read port, and a width that forces a 36Kb primitive rather
// than a LUTRAM.  The `ram_style` attribute is the mechanism LiteX builds
// rely on to keep wide buffers out of distributed RAM -- getting that
// wrong costs thousands of LUTs and, on a large design, router
// convergence.
module top (
    input  wire sysclk_p,
    input  wire sysclk_n,
    input  wire rst,
    output wire [1:0] led
);
    wire clk_raw, clk, rst_buf;
    IBUFDS #(.DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"), .IOSTANDARD("LVDS"))
        ibufds (.I(sysclk_p), .IB(sysclk_n), .O(clk_raw));
    BUFG bufg (.I(clk_raw), .O(clk));
    IBUF ibuf_rst (.I(rst), .O(rst_buf));

    reg [31:0] lfsr = 32'h89ab_cdef;
    always @(posedge clk)
        lfsr <= rst_buf ? 32'h89ab_cdef
                        : {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};

    // 1024 x 64 = 64Kb: spans two 36Kb primitives, so the byte lanes
    // are split across them and a lane-mapping error shows up as a wrong
    // read rather than as a build failure.
    (* ram_style = "block" *)
    reg [63:0] mem [0:1023];

    wire [9:0]  addr_w = lfsr[9:0];
    wire [9:0]  addr_r = lfsr[19:10];
    wire [7:0]  we     = lfsr[27:20];
    wire [63:0] din    = {lfsr, ~lfsr};

    integer b;
    always @(posedge clk)
        for (b = 0; b < 8; b = b + 1)
            if (we[b])
                mem[addr_w][b*8 +: 8] <= din[b*8 +: 8];

    reg [63:0] dout;
    always @(posedge clk) dout <= mem[addr_r];

    reg [1:0] acc;
    always @(posedge clk) acc <= {acc[0], ^dout ^ acc[1]};
    assign led = acc;
endmodule
