// Adaptation of the VC707 Johnson counter to the lowRISC Sonata board.
// Keeps the same 28-bit LFSR + Johnson pattern logic while using the Sonata
// 25 MHz board clock and LEDs.
module johnson_sonata (
    input  wire       mainClk,
    output wire [7:0] usrLed
);
    // x^25 + x^3 + 1 is maximal length: one tick every ~1.34 s at 25 MHz.
    reg  [24:0] prbs = 25'h1;
    wire        fb   = prbs[24] ^ prbs[2];
    wire        tick = (prbs == 25'h1);

    always @(posedge mainClk) begin
        prbs <= {prbs[23:0], fb};
    end

    reg [7:0] johnson = 8'h00;
    always @(posedge mainClk) begin
        if (tick)
            johnson <= {johnson[6:0], ~johnson[7]};
    end

    assign usrLed = johnson;
endmodule
