// Carry positions whose SUM and whose CARRY-OUT both feed fabric logic.
//
// A 7-series subslice has two fabric exits: the hardwired direct pin
// (A/B/C/D, carrying O6 or the carry XOR, with no config bit in prjxray
// because it is not configurable) and the configurable xMUX.  A packer
// that charges both the sum and the carry-out to the xMUX rejects the
// combination as illegal.  Vivado emits it 121 times in 1348 carry bits of
// a design that runs on hardware, so the combination is legal and the two
// exits are genuinely separate.
//
// CARRY4 is instantiated explicitly so the case is guaranteed rather than
// left to synthesis.  Self-contained: an LFSR drives it and everything
// reduces to one LED.
module top (
    input  wire sysclk_p,
    input  wire sysclk_n,
    input  wire rst,
    output wire [0:0] led
);
    localparam N = 8;                  // 8 CARRY4s = 32 carry bits

    wire clk_raw, clk, rst_buf;
    IBUFDS #(.DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"), .IOSTANDARD("LVDS"))
        ibufds (.I(sysclk_p), .IB(sysclk_n), .O(clk_raw));
    BUFG bufg (.I(clk_raw), .O(clk));
    IBUF ibuf_rst (.I(rst), .O(rst_buf));

    reg [31:0] lfsr = 32'h1234_5678;
    always @(posedge clk)
        lfsr <= rst_buf ? 32'h1234_5678
                        : {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};

    wire [N*4-1:0] sum, cout;
    genvar i;
    generate
        for (i = 0; i < N; i = i + 1) begin : blk
            wire [3:0] di = lfsr[(i*3)   +: 4];
            wire [3:0] s  = lfsr[(i*3+7) +: 4] ^ {4{lfsr[i]}};
            CARRY4 u_carry (
                .CO     (cout[i*4 +: 4]),
                .O      (sum [i*4 +: 4]),
                .CI     (1'b0),
                .CYINIT (lfsr[i+2]),
                .DI     (di),
                .S      (s)
            );
        end
    endgenerate

    // Mix each SUM bit with a CARRY bit from the SAME position: both must
    // then leave their slice, which is the case under test.  The rotation
    // stops the result being computable inside one slice.
    reg [N*4-1:0] mixed;
    integer k;
    always @(posedge clk) begin
        for (k = 0; k < N*4; k = k + 1)
            mixed[k] <= sum[k] ^ cout[k] ^ lfsr[k % 32];
    end

    reg acc;
    always @(posedge clk) acc <= ^mixed ^ acc;
    assign led[0] = acc;
endmodule
