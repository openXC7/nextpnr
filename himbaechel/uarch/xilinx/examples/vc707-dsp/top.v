// DSP48E1 multiply-accumulate with a registered pipeline.
//
// Covers the DSP48E1 config writer, which no demo project reaches: the
// blinkies have no multiplier and the LiteX SoCs in the gate are built
// without one.  The register-enable and OPMODE/ALUMODE fields are the
// parts worth exercising, because their encodings are what differ between
// two equivalent-looking configurations.
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

    reg [31:0] lfsr = 32'hfeed_face;
    always @(posedge clk)
        lfsr <= rst_buf ? 32'hfeed_face
                        : {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};

    wire [47:0] p;

    DSP48E1 #(
        .AREG          (1), .BREG (1), .CREG (1),
        .MREG          (1), .PREG (1),
        .USE_MULT      ("MULTIPLY"),
        .USE_DPORT     ("FALSE"),
        .A_INPUT       ("DIRECT"),
        .B_INPUT       ("DIRECT")
    ) dsp (
        .CLK      (clk),
        .A        ({{5{lfsr[24]}}, lfsr[24:0]}),
        .B        (lfsr[17:0]),
        .C        ({{16{lfsr[31]}}, lfsr}),
        .D        (25'b0),
        .P        (p),
        .OPMODE   (7'b0110101),   // P = C + M (multiply-accumulate)
        .ALUMODE  (4'b0000),      // Z + X + Y + CIN
        .INMODE   (5'b00000),
        .CARRYIN  (1'b0),
        .CARRYINSEL (3'b000),
        .CEA1 (1'b0), .CEA2 (1'b1),
        .CEB1 (1'b0), .CEB2 (1'b1),
        .CEC  (1'b1), .CED  (1'b0), .CEM (1'b1), .CEP (1'b1),
        .CEAD (1'b0), .CEALUMODE (1'b1), .CECARRYIN (1'b0),
        .CECTRL (1'b1), .CEINMODE (1'b1),
        .RSTA (rst_buf), .RSTB (rst_buf), .RSTC (rst_buf), .RSTD (rst_buf),
        .RSTM (rst_buf), .RSTP (rst_buf), .RSTALLCARRYIN (rst_buf),
        .RSTALUMODE (rst_buf), .RSTCTRL (rst_buf), .RSTINMODE (rst_buf),
        .ACIN (30'b0), .BCIN (18'b0), .PCIN (48'b0),
        .MULTSIGNIN (1'b0)
    );

    reg [1:0] acc;
    always @(posedge clk) acc <= {acc[0], ^p ^ acc[1]};
    assign led = acc;
endmodule
