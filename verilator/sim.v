`timescale 1 ps / 1 ps

module emu(

	input clk_sys /*verilator public_flat*/,
	input RESET /*verilator public_flat*/,
	input [13:0]  inputs/*verilator public_flat*/,

	output [7:0] VGA_R/*verilator public_flat*/,
	output [7:0] VGA_G/*verilator public_flat*/,
	output [7:0] VGA_B/*verilator public_flat*/,
	
	output VGA_HS,
	output VGA_VS,
	output VGA_HB,
	output VGA_VB,

	output [15:0] AUDIO_L,
	output [15:0] AUDIO_R,

	input        ioctl_download,
	input        ioctl_upload,
	input        ioctl_wr,
	input [24:0] ioctl_addr,
	input [7:0]  ioctl_dout,
	input [7:0]  ioctl_din,
	input [7:0]  ioctl_index,
	output  reg  ioctl_wait=1'b0

);

	reg ce_pix /*verilator public_flat*/;
	reg pause_cpu /*verilator public_flat*/;

	// Inputs
	wire bCabinet  = 1'b0;	// (upright only)
	wire m_right1 = inputs[0];
	wire m_left1 = inputs[1];
	wire m_down1 = inputs[2];
	wire m_up1 = inputs[3];
	wire m_right2 = inputs[4];
	wire m_left2 = inputs[5];
	wire m_down2 = inputs[6];
	wire m_up2 = inputs[7];
	wire m_coin1 = inputs[8];
	wire m_coin2 = inputs[9];
	wire m_start1 = inputs[10];
	wire m_start2 = inputs[11];
	wire m_trig1 = inputs[12];
	wire m_trig2 = inputs[13];

	wire		PCLK;
	wire  [8:0] HPOS,VPOS;
	wire [11:0] POUT;
	HVGEN hvgen
	(
		.HPOS(HPOS),.VPOS(VPOS),.PCLK(PCLK),.iRGB(POUT),
		.oRGB({b,g,r}),.HBLK(hblank),.VBLK(vblank),.HSYN(hs),.VSYN(vs)
	);
	assign ce_vid = PCLK;
	
	always @(posedge clk_sys) begin
		reg old_clk;
		old_clk <= ce_vid;
		ce_pix  <= old_clk & ~ce_vid;
	end

	// Convert video output to 8bpp RGB
	assign VGA_R = {2{r}};
	assign VGA_G = {2{g}};
	assign VGA_B = {2{b}};

	wire ce_vid;
	wire hblank, vblank;
	wire hs, vs;
	wire [3:0] r,g,b;

	assign VGA_HB = hblank;
	assign VGA_HS = hs;
	assign VGA_VB = vblank;
	assign VGA_VS = vs;
	
	reg rom_downloaded = 0;
	wire rom_download = ioctl_download && ioctl_index == 8'b0;

	wire reset/*verilator public_flat*/;
	assign reset = (RESET | rom_download | !rom_downloaded); 
	always @(posedge clk_sys) if(rom_download) rom_downloaded <= 1'b1; // Latch downloaded rom state to release reset

	wire  [1:0] COIA = 2'b00;			// 1coin/1credit
	wire  [2:0] COIB = 3'b001;			// 1coin/1credit
	wire		CABI = ~bCabinet;
	wire		FRZE = 1'b1;

	wire	[1:0] DIFC = 2'b00;
	wire  [1:0] LIFE = 2'b00;
	wire  [2:0] EXMD = 3'b00;
	wire			CONT = 1'b1;
	wire			DSND = 1'b1;
	wire     SERVICE = 1'b0;

	reg 			V_FLIP/*verilator public_flat*/;

	wire  [7:0] DSW0 = {LIFE,EXMD,COIB};
	wire  [7:0] DSW1 = {COIA,FRZE,DSND,CONT,CABI,DIFC};
	wire  [7:0] INP0 = {SERVICE, 1'b0, m_coin2, m_coin1, m_start2, m_start1, m_trig2, m_trig1 };
	wire  [7:0] INP1 = {m_left2, m_down2, m_right2, m_up2, m_left1, m_down1, m_right1, m_up1 };

	wire  [7:0] oPIX;
	wire  [7:0] oSND;

	// always @(posedge clk_sys)
	// begin
	// 	$display("PCLK: %b  HPOS: %d VPOS: %d", PCLK, HPOS, VPOS);
	// end

	FPGA_DIGDUG GameCore ( 
		.RESET(reset),.MCLK(clk_sys),
		.INP0(INP0),.INP1(INP1),.DSW0(DSW0),.DSW1(DSW1),
		.PH(HPOS),.PV(VPOS),.PCLK(PCLK),.POUT(oPIX),
		.SOUT(oSND),
		
		.LED(),

		.V_FLIP(V_FLIP),

		.ROMCL(clk_sys),.ROMAD(ioctl_addr[15:0]),.ROMDT(ioctl_dout),.ROMEN(ioctl_wr & rom_download),

		.PAUSE(pause_cpu),

		.hs_address(),
		.hs_data_in(),
		.hs_data_out(),
		.hs_write(),
		.hs_access()
	);

	assign POUT = {oPIX[7:6],2'b00,oPIX[5:3],1'b0,oPIX[2:0],1'b0};
	//assign AOUT = {oSND,8'h0};

endmodule
