#include <verilated.h>
#include "Vemu.h"

#include "imgui.h"
#include "implot.h"
#ifndef _MSC_VER
#include <stdio.h>
#include <SDL.h>
#include <SDL_opengl.h>
#else
#define WIN32
#include <dinput.h>
#endif

#include "sim_console.h"
#include "sim_bus.h"
#include "sim_video.h"
#include "sim_audio.h"
#include "sim_input.h"
#include "sim_clock.h"

#include "../imgui/imgui_memory_editor.h"
#include "../imgui/ImGuiFileDialog.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <iterator>
#include <string>
#include <sstream>
#include <iomanip>

using namespace std;

// Simulation control
// ------------------
int initialReset = 48;
bool run_enable = 1;
bool pause_game = 0;
int batchSize = 25000000 / 100;
bool single_step = 0;
bool multi_step = 0;
int multi_step_amount = 1024;
bool v_flip = 0;


// Debug GUI 
// ---------
const char* windowTitle = "Verilator Sim: Sega/Gremlin - VIC";
const char* windowTitle_Control = "Simulation control";
const char* windowTitle_DebugLog = "Debug log";
const char* windowTitle_Video = "VGA output";
const char* windowTitle_Audio = "Audio output";
bool showDebugLog = true;
DebugConsole console;
MemoryEditor mem_edit_8;
MemoryEditor mem_edit_16;

// HPS emulator
// ------------
SimBus bus(console);

// Input handling
// --------------
SimInput input(13, console);

const int input_p1_right = 0;
const int input_p1_left = 1;
const int input_p1_down = 2;
const int input_p1_up = 3;

const int input_p2_right = 4;
const int input_p2_left = 5;
const int input_p2_down = 6;
const int input_p2_up = 7;

const int input_coin1 = 8;
const int input_coin2 = 9;
const int input_start1 = 10;
const int input_start2 = 11;
const int input_fire1 = 12;
const int input_fire2 = 13;

// Video
// -----
#define VGA_WIDTH 240
#define VGA_HEIGHT 288
#define VGA_ROTATE 1  // 90 degrees anti-clockwise
#define VGA_SCALE_X vga_scale
#define VGA_SCALE_Y vga_scale
SimVideo video(VGA_WIDTH, VGA_HEIGHT, VGA_ROTATE);
float vga_scale = 2.0;

// Verilog module
// --------------
Vemu* top = NULL;
vluint64_t main_time = 0; // Current simulation time.
double sc_time_stamp()
{ // Called by $time in Verilog.
	return main_time;
}

int clk_sys_freq = 15468480;
SimClock clk_sys(1);

void resetSim()
{
	main_time = 0;
	top->RESET = 1;
	clk_sys.Reset();
}


// Audio
// -----
#define DISABLE_AUDIO
#ifndef DISABLE_AUDIO
SimAudio audio(clk_sys_freq, true);
#endif


// MAME debug log
//#define CPU_DEBUG

#ifdef CPU_DEBUG
bool log_instructions = true;
bool stop_on_log_mismatch = true;


std::vector<std::string> log_mame;
std::vector<std::string> log_cpu;
long log_index;
unsigned int ins_count = 0;

// CPU debug
bool cpu_sync;
bool cpu_sync_last;
std::vector<std::vector<std::string> > opcodes;
std::map<std::string, std::string> opcode_lookup;

bool writeLog(const char* line)
{
	// Write to cpu log
	log_cpu.push_back(line);

	// Compare with MAME log
	bool match = true;
	ins_count++;

	std::string c_line = std::string(line);

	std::string c = "%d > " + c_line;

	if (log_index < log_mame.size()) {
		std::string m_line = log_mame.at(log_index);
		if (log_instructions) {
			console.AddLog(c.c_str(), ins_count);
		}

		if (stop_on_log_mismatch && m_line != c_line) {
			console.AddLog("DIFF at %d", log_index);
			std::string m = "MAME > " + m_line;
			console.AddLog(m.c_str());
			console.AddLog(c.c_str(), ins_count);
			match = false;
			run_enable = 0;
		}
	}
	else {
		console.AddLog("MAME OUT");
		run_enable = 0;
	}

	log_index++;
	return match;

}

void loadOpcodes()
{
	std::string fileName = "z80_opcodes.csv";

	std::string                           header;
	std::ifstream                         reader(fileName);
	if (reader.is_open()) {
		std::string line, column, id;
		std::getline(reader, line);
		header = line;
		while (std::getline(reader, line)) {
			std::stringstream        ss(line);
			std::vector<std::string> columns;
			bool                     withQ = false;
			std::string              part{ "" };
			while (std::getline(ss, column, ',')) {
				auto pos = column.find("\"");
				if (pos < column.length()) {
					withQ = !withQ;
					part += column.substr(0, pos);
					column = column.substr(pos + 1, column.length());
				}
				if (!withQ) {
					column += part;
					columns.emplace_back(std::move(column));
					part = "";
				}
				else {
					part += column + ",";
				}
			}
			opcodes.push_back(columns);
			opcode_lookup[columns[0]] = columns[1];
		}
	}
};

std::string int_to_hex(unsigned char val)
{
	std::stringstream ss;
	ss << std::setfill('0') << std::setw(2) << std::hex << (val | 0);
	return ss.str();
}

std::string get_opcode(int ir, int ir_ext)
{
	std::string hex = "0x";
	if (ir_ext > 0) {
		hex.append(int_to_hex(ir_ext));
	}
	hex.append(int_to_hex(ir));
	if (opcode_lookup.find(hex) != opcode_lookup.end()) {
		return opcode_lookup[hex];
	}
	else
	{
		hex.append(" - MISSING OPCODE");
		return hex;
	}
}

bool hasEnding(std::string const& fullString, std::string const& ending) {
	if (fullString.length() >= ending.length()) {
		return (0 == fullString.compare(fullString.length() - ending.length(), ending.length(), ending));
	}
	else {
		return false;
	}
}

std::string last_log;
//
unsigned short last_pc;
unsigned short last_last_pc;
unsigned char last_mreq;

unsigned short active_pc;
unsigned char active_ir;
unsigned char active_ir_ext;

const int ins_size = 48;
int ins_index = 0;
int ins_pc[ins_size];
int ins_in[ins_size];
int ins_ma[ins_size];
unsigned char active_ins = 0;

bool vbl_last;

#endif

void saveScreenshot() {

}

int verilate()
{

	if (!Verilated::gotFinish())
	{

		// Assert reset during startup
		if (main_time < initialReset) { top->RESET = 1; }
		// Deassert reset after startup
		if (main_time == initialReset) { top->RESET = 0; }

		// Clock dividers
		clk_sys.Tick();

		// Set system clock in core
		top->clk_sys = clk_sys.clk;

		// Simulate both edges of system clock
		if (clk_sys.clk != clk_sys.old) {
			if (clk_sys.clk) {
				input.BeforeEval();
				bus.BeforeEval();
			}
			top->eval();
			if (clk_sys.clk) { bus.AfterEval(); }


		}

#ifndef DISABLE_AUDIO
		if (clk_sys.IsRising())
		{
			audio.Clock(top->AUDIO_L, top->AUDIO_R);
		}
#endif

		// Output pixels on rising edge of pixel clock
		if (clk_sys.IsRising() && top->emu__DOT__ce_pix) {
			uint32_t colour = 0xFF000000 | top->VGA_B << 16 | top->VGA_G << 8 | top->VGA_R;
			video.Clock(top->VGA_HB, top->VGA_VB, top->VGA_HS, top->VGA_VS, colour);
		}

		if (clk_sys.IsRising()) {


#ifdef CPU_DEBUG
			if (!top->emu__DOT__system__DOT__reset) {


				unsigned short pc = top->emu__DOT__system__DOT__cpu__DOT__i_tv80_core__DOT__PC;

				unsigned char di = top->emu__DOT__system__DOT__cpu__DOT__i_tv80_core__DOT__di;
				unsigned short ad = top->emu__DOT__system__DOT__cpu__DOT__i_tv80_core__DOT__A;
				unsigned char ir = top->emu__DOT__system__DOT__cpu__DOT__i_tv80_core__DOT__IR;

				unsigned char phi = top->emu__DOT__system__DOT__cpu__DOT__cen;
				unsigned char mcycle = top->emu__DOT__system__DOT__cpu__DOT__mcycle;
				unsigned char mreq = top->emu__DOT__system__DOT__cpu__DOT__mreq_n;
				bool ir_changed = top->emu__DOT__system__DOT__cpu__DOT__i_tv80_core__DOT__ir_changed;
				top->emu__DOT__system__DOT__cpu__DOT__i_tv80_core__DOT__ir_changed = 0;

				bool new_data = (mreq && !last_mreq && mcycle <= 4);
				if (new_data && !ir_changed) {
					//console.AddLog("%08d NEW DATA> PC=%04x IR=%02x AD=%04x DI=%02x M=%x LM=%x MR=%x", main_time, pc, ir, ad, di, mcycle, last_mcycle, mreq);

					ins_in[ins_index] = di;
					ins_index++;
					if (ins_index > ins_size - 1) { ins_index = 0; }
				}

				last_mreq = mreq;

				if (ir_changed) {
					//console.AddLog("IR_CHANGED--> %08d PC> PC=%04x IR=%02x AD=%04x DI=%02x M=%x MR=%x", main_time, pc, ir, ad, di, mcycle, mreq);

					//console.AddLog("ACTIVE_IR: %x ACTIVE_PC: %x", active_ir, active_pc);

					if (active_ir > 0) {
						std::string opcode = get_opcode(active_ir, 0);

						if (opcode.c_str() == "")
						{
							console.AddLog("No opcode found for %x", active_ir);
						}

						// Is this a compound opcode?
						size_t pos = opcode.find("****");
						if (pos != std::string::npos)
						{
							active_ir_ext = active_ir;
						}
						else {
							unsigned char data1 = ins_in[ins_index - 2];
							unsigned char data2 = ins_in[ins_index - 1];
							std::string fmt = "%04X: ";
							std::string opcode = get_opcode(active_ir, active_ir_ext);

							if (active_ir_ext>0) {
						//		console.AddLog("EXTINS: %x%x %s", active_ir_ext, active_ir, opcode.c_str());
							}
							else {
							//	console.AddLog("INS: %x %s", active_ir, opcode.c_str());
							}

							size_t pos = opcode.find("&0000");
							if (pos != std::string::npos)
							{
								char buf[6];
								sprintf(buf, "$%02X%02X", data2, data1);
								opcode.replace(pos, 5, buf);
							}

							pos = opcode.find("&4546");
							if (pos != std::string::npos)
							{
								char buf[6];
								short add = active_pc + data2 + 2;
								if (opcode.substr(0, 4) == "djnz") {
									add = active_pc + ((signed char)data2) + 2;
								}
								if (opcode.substr(0, 4) == "jr  ") {
									add = active_pc + ((signed char)data2) + 2;
								}
								sprintf(buf, "$%04X", add);
								opcode.replace(pos, 5, buf);
							}

							pos = opcode.find("&00");
							if (pos != std::string::npos)
							{
								for (int i = 0; i < ins_index; i++) {
									console.AddLog("ins %d = %x", i, ins_in[i]);
								}
								//console.AddLog("data1=%x data2=%x ins_index=%d", data1, data2, ins_index);
								unsigned char data = data2;
								if (opcode.find("),") != std::string::npos) {
									//console.AddLog("Single byte, second part? %s", opcode.c_str());

									if (opcode.find("(&00)") == std::string::npos)
									{
										//console.AddLog("");
										data = data1;
									}
								}
								char buf[4];
								sprintf(buf, "$%02X", data);
								opcode.replace(pos, 3, buf);
							}

							fmt.append(opcode);
							char buf[1024];
							sprintf(buf, fmt.c_str(), active_pc);
							writeLog(buf);

							// Clear instruction cache
							ins_index = 0;
							for (int i = 0; i < ins_size; i++) {
								ins_in[i] = 0;
								ins_ma[i] = 0;
							}
							active_ir_ext = 0;
							active_pc = ad;
						}
					}
					//console.AddLog("Setting active last_last_pc=%x last_pc=%x pc=%x addr=%x", last_last_pc, last_pc, pc, ad);

					active_ir = ir;

					last_last_pc = last_pc;
					last_pc = pc;
				}
			}
#endif

			main_time++;
		}
		return 1;
		}

	// Stop verilating and cleanup
	top->final();
	delete top;
	exit(0);
	return 0;
	}

int main(int argc, char** argv, char** env)
{
	// Create core and initialise
	top = new Vemu();
	Verilated::commandArgs(argc, argv);

#ifdef WIN32
	// Attach debug console to the verilated code
	Verilated::setDebug(console);
#endif

#ifdef CPU_DEBUG
	// Load debug opcodes
	loadOpcodes();

	// Load debug trace
	std::string line;
	std::ifstream fin(tracefilename);
	while (getline(fin, line)) {
		log_mame.push_back(line);
	}
#endif

	// Attach bus
	bus.ioctl_addr = &top->ioctl_addr;
	bus.ioctl_index = &top->ioctl_index;
	bus.ioctl_wait = &top->ioctl_wait;
	bus.ioctl_download = &top->ioctl_download;
	//bus.ioctl_upload = &top->ioctl_upload;
	bus.ioctl_wr = &top->ioctl_wr;
	bus.ioctl_dout = &top->ioctl_dout;
	//bus.ioctl_din = &top->ioctl_din;
	//input.ps2_key = &top->ps2_key;

#ifndef DISABLE_AUDIO
	audio.Initialise();
#endif

	mem_edit_16.Cols = 32;

	// Set up input module
	input.Initialise();
#ifdef WIN32
	input.SetMapping(input_p1_up, DIK_UP);
	input.SetMapping(input_p1_right, DIK_RIGHT);
	input.SetMapping(input_p1_down, DIK_DOWN);
	input.SetMapping(input_p1_left, DIK_LEFT);
	input.SetMapping(input_p2_up, DIK_W);
	input.SetMapping(input_p2_right, DIK_D);
	input.SetMapping(input_p2_down, DIK_S);
	input.SetMapping(input_p2_left, DIK_A);
	input.SetMapping(input_coin1, DIK_5);
	input.SetMapping(input_coin2, DIK_6);
	input.SetMapping(input_start1, DIK_1);
	input.SetMapping(input_start2, DIK_2);
	input.SetMapping(input_fire1, DIK_RCONTROL);
	input.SetMapping(input_fire2, DIK_LCONTROL);
#else
	input.SetMapping(input_p1_up, SDL_SCANCODE_UP);
	input.SetMapping(input_p1_right, SDL_SCANCODE_RIGHT);
	input.SetMapping(input_p1_down, SDL_SCANCODE_DOWN);
	input.SetMapping(input_p1_left, SDL_SCANCODE_LEFT);
	input.SetMapping(input_p2_up, SDL_SCANCODE_W);
	input.SetMapping(input_p2_right, SDL_SCANCODE_D);
	input.SetMapping(input_p2_down, SDL_SCANCODE_S);
	input.SetMapping(input_p2_left, SDL_SCANCODE_A);
	input.SetMapping(input_coin1, SDL_SCANCODE_5);
	input.SetMapping(input_coin2, SDL_SCANCODE_6);
	input.SetMapping(input_start1, SDL_SCANCODE_1);
	input.SetMapping(input_start2, SDL_SCANCODE_2);
	input.SetMapping(input_fire1, SDL_SCANCODE_RCONTROL);
	input.SetMapping(input_fire2, SDL_SCANCODE_LCONTROL);

#endif

	// Stage ROMs

	bus.QueueDownload("roms/dd1a.1",0);
	bus.QueueDownload("roms/dd1a.2", 0);
	bus.QueueDownload("roms/dd1a.3", 0);
	bus.QueueDownload("roms/dd1a.4", 0);
	bus.QueueDownload("roms/dd1.15", 0);
	bus.QueueDownload("roms/dd1.14", 0);
	bus.QueueDownload("roms/dd1.13", 0);
	bus.QueueDownload("roms/dd1.12", 0);
	bus.QueueDownload("roms/dd1a.5", 0);
	bus.QueueDownload("roms/dd1a.6", 0);
	bus.QueueDownload("roms/dd1.7", 0);
	bus.QueueDownload("roms/dd1.10b", 0);
	bus.QueueDownload("roms/dd1.11", 0);
	bus.QueueDownload("roms/dd1.9", 0);
	bus.QueueDownload("roms/136007.110", 0);
	bus.QueueDownload("roms/136007.111", 0);
	bus.QueueDownload("roms/136007.112", 0);
	bus.QueueDownload("roms/136007.113", 0);

	// Setup video output
	if (video.Initialise(windowTitle) == 1) { return 1; }

#ifdef WIN32
	MSG msg;
	ZeroMemory(&msg, sizeof(msg));
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			continue;
		}
#else
	bool done = false;
	while (!done)
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			ImGui_ImplSDL2_ProcessEvent(&event);
			if (event.type == SDL_QUIT)
				done = true;
		}
#endif
		video.StartFrame();

		input.Read();


		// Draw GUI
		// --------
		ImGui::NewFrame();

		// Simulation control window
		ImGui::Begin(windowTitle_Control);
		ImGui::SetWindowPos(windowTitle_Control, ImVec2(0, 0), ImGuiCond_Once);
		ImGui::SetWindowSize(windowTitle_Control, ImVec2(500, 170), ImGuiCond_Once);
		if (ImGui::Button("Reset simulation")) { resetSim(); } ImGui::SameLine();
		if (ImGui::Button("Start running")) { run_enable = 1; } ImGui::SameLine();
		if (ImGui::Button("Stop running")) { run_enable = 0; } ImGui::SameLine();
		ImGui::Checkbox("RUN", &run_enable);
		//ImGui::PopItemWidth();
		ImGui::SliderInt("Run batch size", &batchSize, 1, 250000);
		if (single_step == 1) { single_step = 0; }
		if (ImGui::Button("Single Step")) { run_enable = 0; single_step = 1; }
		ImGui::SameLine();
		if (multi_step == 1) { multi_step = 0; }
		if (ImGui::Button("Multi Step")) { run_enable = 0; multi_step = 1; }
		//ImGui::SameLine();
		ImGui::SliderInt("Multi step amount", &multi_step_amount, 8, 1024);

		ImGui::Checkbox("Pause game", &pause_game); ImGui::SameLine();
		top->emu__DOT__pause_cpu = pause_game;
		ImGui::Checkbox("Vertical Flip", &v_flip); ImGui::SameLine();
		top->emu__DOT__V_FLIP = v_flip;
		
		//if (ImGui::Button("Save screenshot")) { saveScreenshot(); }

#ifdef CPU_DEBUG
		ImGui::NewLine();
		ImGui::Checkbox("Log CPU instructions", &log_instructions);
		ImGui::Checkbox("Stop on MAME diff", &stop_on_log_mismatch);
#endif

		ImGui::End();

		// Debug log window
		console.Draw(windowTitle_DebugLog, &showDebugLog, ImVec2(500, 700));
		ImGui::SetWindowPos(windowTitle_DebugLog, ImVec2(0, 160), ImGuiCond_Once);

		// Memory debug
		//ImGui::Begin("ROM1");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__roms__DOT__rom1__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("ROM2");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__roms__DOT__rom2__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("ROM3");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__roms__DOT__rom3__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("ROM4");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__roms__DOT__rom4__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("ROM5");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__roms__DOT__rom5__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("ROM6");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__roms__DOT__rom6__DOT__mem, 1024, 0);
		//ImGui::End();
		//ImGui::Begin("PROM1");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__prom1__DOT__mem, 32, 0);
		//ImGui::End();
		//ImGui::Begin("PROM2");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__prom2__DOT__mem, 32, 0);
		//ImGui::End();
		//ImGui::Begin("RAM");
		//mem_edit_16.DrawContents(&top->emu__DOT__system__DOT__ram__DOT__mem, 4096, 0);
		//ImGui::End();
		//ImGui::Begin("COLPROM");
		//mem_edit_8.DrawContents(&top->emu__DOT__system__DOT__colprom__DOT__mem, 32, 0);
		//ImGui::End();
		int windowX = 550;
		int windowWidth = (VGA_WIDTH * VGA_SCALE_X) + 24;
		int windowHeight = (VGA_HEIGHT * VGA_SCALE_Y) + 90;

		// Video window
		ImGui::Begin(windowTitle_Video);
		ImGui::SetWindowPos(windowTitle_Video, ImVec2(windowX, 0), ImGuiCond_Once);
		ImGui::SetWindowSize(windowTitle_Video, ImVec2(windowWidth, windowHeight), ImGuiCond_Once);

		ImGui::SetNextItemWidth(400);
		ImGui::SliderFloat("Zoom", &vga_scale, 1, 8); ImGui::SameLine();
		ImGui::SetNextItemWidth(200);
		ImGui::SliderInt("Rotate", &video.output_rotate, -1, 1); ImGui::SameLine();
		ImGui::Checkbox("Flip V", &video.output_vflip);
		ImGui::Text("main_time: %d frame_count: %d sim FPS: %f", main_time, video.count_frame, video.stats_fps);

		// Draw VGA output
		ImGui::Image(video.texture_id, ImVec2(video.output_width * VGA_SCALE_X, video.output_height * VGA_SCALE_Y));
		ImGui::End();


#ifndef DISABLE_AUDIO

		ImGui::Begin(windowTitle_Audio);
		ImGui::SetWindowPos(windowTitle_Audio, ImVec2(windowX, windowHeight), ImGuiCond_Once);
		ImGui::SetWindowSize(windowTitle_Audio, ImVec2(windowWidth, 250), ImGuiCond_Once);
		if (run_enable) {
			audio.CollectDebug((signed short)top->AUDIO_L, (signed short)top->AUDIO_R);
		}
		int channelWidth = (windowWidth / 2) - 16;
		ImPlot::CreateContext();
		if (ImPlot::BeginPlot("Audio - L", ImVec2(channelWidth, 220), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
			ImPlot::SetupAxes("T", "A", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks);
			ImPlot::SetupAxesLimits(0, 1, -1, 1, ImPlotCond_Once);
			ImPlot::PlotStairs("", audio.debug_positions, audio.debug_wave_l, audio.debug_max_samples, audio.debug_pos);
			ImPlot::EndPlot();
		}
		ImGui::SameLine();
		if (ImPlot::BeginPlot("Audio - R", ImVec2(channelWidth, 220), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoTitle)) {
			ImPlot::SetupAxes("T", "A", ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks);
			ImPlot::SetupAxesLimits(0, 1, -1, 1, ImPlotCond_Once);
			ImPlot::PlotStairs("", audio.debug_positions, audio.debug_wave_r, audio.debug_max_samples, audio.debug_pos);
			ImPlot::EndPlot();
		}
		ImPlot::DestroyContext();
		ImGui::End();
#endif

		video.UpdateTexture();

		// Pass inputs to sim
		top->inputs = 0;
		for (int i = 0; i < input.inputCount; i++)
		{
			if (input.inputs[i])
			{
				top->inputs |= (1 << i);
			}
		}

		// Run simulation
		if (run_enable) {
			for (int step = 0; step < batchSize; step++) { verilate(); }
		}
		else {
			if (single_step) { verilate(); }
			if (multi_step) {
				for (int step = 0; step < multi_step_amount; step++) { verilate(); }
			}
		}
	}

	// Clean up before exit
	// --------------------

#ifndef DISABLE_AUDIO
	audio.CleanUp();
#endif
	video.CleanUp();
	input.CleanUp();

	return 0;
}
