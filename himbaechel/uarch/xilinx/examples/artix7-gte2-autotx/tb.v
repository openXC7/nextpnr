module tb;
 reg clk=0;
 always #4 clk=~clk;
 wire tx,hb;
 openxc7_autotx_core dut(.clk(clk),.tx(tx),.heartbeat(hb));
 integer frame,bitno,cycle;
 reg expected;
 initial begin
  for(frame=0;frame<3;frame=frame+1)
   for(bitno=0;bitno<12;bitno=bitno+1) begin
    expected=!(bitno==0 || bitno==2 || bitno==4 || bitno==6 || bitno==8);
    for(cycle=0;cycle<1085;cycle=cycle+1) begin
     @(negedge clk);
     // Last edge advances to next symbol; inspect preceding cycles.
     if(cycle<1084 && tx!==expected) $fatal(1,"frame=%0d bit=%0d cycle=%0d",frame,bitno,cycle);
    end
   end
  $display("PASS: three autonomous 0x55 frames, 1085 cycles/symbol");
  $finish;
 end
endmodule
