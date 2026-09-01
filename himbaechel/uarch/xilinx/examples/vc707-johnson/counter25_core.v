module counter25_core (
    input wire clk,
    input wire rst,
    output wire [7:0] led
);
    reg [27:0] prbs = 28'h1;
    wire fb = prbs[27] ^ prbs[2];
    wire tick = (prbs == 28'h1);

    always @(posedge clk)
        if (rst)
            prbs <= 28'h1;
        else
            prbs <= {prbs[26:0], fb};

    reg [7:0] johnson = 8'h00;
    always @(posedge clk)
        if (rst)
            johnson <= 8'h00;
        else if (tick)
            johnson <= {johnson[6:0], ~johnson[7]};

    assign led = johnson;
endmodule