// Autonomous 8N1 0x55 transmitter. No RX, CPU, memory, PLL or MMCM.
// Qualified Kasli v1.1 Si5324 reference: F10/E10, 125 MHz.
module openxc7_autotx_core #(parameter DIVISOR=1085)(input clk, output tx, output heartbeat);
 reg [10:0] tick=0;
 reg [3:0] phase=0;
 reg [26:0] count=0;
 always @(posedge clk) begin
  count <= count+1;
  if(tick == DIVISOR-1) begin
   tick <= 0;
   if(phase == 11) phase <= 0; else phase <= phase+1;
  end else tick <= tick+1;
 end
 // start, eight alternating data bits LSB first, stop, two idle bits.
 assign tx = (phase==0 || phase==2 || phase==4 || phase==6 || phase==8) ? 1'b0 : 1'b1;
 assign heartbeat=count[26];
endmodule
module openxc7_autotx(input clk125_p, input clk125_n, output uart_tx,
 output led_heartbeat, output [2:0] sfp_tx_disable);
 wire ref125, clk;
 IBUFDS_GTE2 input_clock(.I(clk125_p),.IB(clk125_n),.CEB(1'b0),.O(ref125),.ODIV2());
 BUFG reference_buffer(.I(ref125),.O(clk));
 openxc7_autotx_core transmitter(.clk(clk),.tx(uart_tx),.heartbeat(led_heartbeat));
 assign sfp_tx_disable=3'b111;
endmodule
