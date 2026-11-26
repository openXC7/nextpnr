// One slice whose main flip-flop takes its D over the bypass (BX) while the
// 5FF beside it is in use.
//
//   ff_main  BFF   D = cnt[5], from a flip-flop in another slice: the only
//                  way into BFF is the BX bypass (BFFMUX = BX)
//   lut_five B5LUT an inverter of cnt[6]
//   ff_five  B5FF  D = lut_five's O5
//
// The three are pinned to SLICE_X4Y120 by top.xdc (vivado.xdc for Vivado).
// Both flip-flops share the slice's clock, CE and SR, so the slice is legal;
// Vivado places and routes it, and uses the same pairing on its own (25 times
// in a 1000-cell design measured on a Cmod A7).
//
// Self-check: each cycle, ff_main must hold the previous cnt[5] and ff_five
// the inverse of the previous cnt[6]. led[1] lights and stays lit on the
// first mismatch (e.g. a BFF whose D was left floating). led[0] blinks at
// about 0.7 Hz to show the clock runs.

module top (
    input  wire       clk,      // 12 MHz on the Cmod A7
    output wire [1:0] led
);
    reg [23:0] cnt = 24'd0;
    always @(posedge clk)
        cnt <= cnt + 1'b1;

    wire main_q, five_d, five_q;

    (* keep *) FDRE #(.INIT(1'b0)) ff_main (
        .C(clk), .CE(1'b1), .R(1'b0), .D(cnt[5]), .Q(main_q));

    (* keep *) LUT1 #(.INIT(2'b01)) lut_five (.I0(cnt[6]), .O(five_d));

    (* keep *) FDRE #(.INIT(1'b0)) ff_five (
        .C(clk), .CE(1'b1), .R(1'b0), .D(five_d), .Q(five_q));

    // cnt - 1 is the value cnt had when ff_main and ff_five last sampled it
    wire [23:0] prev = cnt - 1'b1;
    reg armed = 1'b0;       // the flip-flops' first sample is one cycle in
    reg err   = 1'b0;
    always @(posedge clk) begin
        armed <= 1'b1;
        if (armed && (main_q != prev[5] || five_q != ~prev[6]))
            err <= 1'b1;
    end

    assign led = {err, cnt[23]};
endmodule
