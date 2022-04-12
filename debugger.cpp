#include "bus.h"
#include "console.h"
#include "cpu.h"
#include "utils.h"


#if defined(ESP32)
void setBootLoader(bus *const b);
#endif

extern uint32_t         event;
extern std::atomic_bool terminate;

// returns size of instruction (in bytes)
int disassemble(cpu *const c, console *const cnsl, const int pc, const bool instruction_only)
{
	auto data      = c->disassemble(pc);

	auto registers = data["registers"];
	auto psw       = data["psw"][0];

	std::string instruction_values;
	for(auto iv : data["instruction-values"])
		instruction_values += (instruction_values.empty() ? "" : ",") + iv;

	std::string work_values;
	for(auto wv : data["work-values"])
		work_values += (work_values.empty() ? "" : ",") + wv;

	std::string instruction = data["instruction-text"].at(0);

	std::string result;

	if (instruction_only)
		result = format("PC: %06o, instr: %s\t%s\t%s",
				pc,
				instruction_values.c_str(),
				instruction.c_str(),
				work_values.c_str()
				);
	else
		result = format("R0: %s, R1: %s, R2: %s, R3: %s, R4: %s, R5: %s, SP: %s, PC: %06o, PSW: %s, instr: %s: %s - %s",
				registers[0].c_str(), registers[1].c_str(), registers[2].c_str(), registers[3].c_str(), registers[4].c_str(), registers[5].c_str(),
				registers[6].c_str(), pc, 
				psw.c_str(),
				instruction_values.c_str(),
				instruction.c_str(),
				work_values.c_str()
				);

	if (cnsl)
		cnsl->debug(result);
	else
		fprintf(stderr, "%s\n", result.c_str());

	return data["instruction-values"].size() * 2;
}

std::map<std::string, std::string> split(const std::vector<std::string> & kv_array, const std::string & splitter)
{
	std::map<std::string, std::string> out;

	for(auto pair : kv_array) {
		auto kv = split(pair, splitter);

		if (kv.size() == 1)
			out.insert({ kv[0], "" });
		else if (kv.size() == 2)
			out.insert({ kv[0], kv[1] });
	}

	return out;
}

void debugger(console *const cnsl, bus *const b, std::atomic_bool *const interrupt_emulation, const bool tracing)
{
	cpu *const c = b->getCpu();

	bool single_step = false;

	while(!terminate) {
		bool        temp  = terminate;
		std::string cmd   = cnsl->read_line(format("%d%d", event, temp));
		auto        parts = split(cmd, " ");
		auto        kv    = split(parts, "=");

		if (parts.empty())
			continue;

		if (cmd == "go")
			single_step = false;
		else if (cmd == "single" || cmd == "s")
			single_step = true;
		else if ((parts[0] == "sbp" || parts[0] == "cbp") && parts.size() == 2){
			uint16_t pc = std::stoi(parts[1].c_str(), nullptr, 8);

			if (parts[0] == "sbp") {
				c->set_breakpoint(pc);

				cnsl->put_string_lf(format("Set breakpoint at %06o", pc));
			}
			else {
				c->remove_breakpoint(pc);

				cnsl->put_string_lf(format("Clear breakpoint at %06o", pc));
			}

			continue;
		}
		else if (cmd == "lbp") {
			auto bps = c->list_breakpoints();

			cnsl->put_string_lf("Breakpoints:");

			for(auto pc : bps) {
				cnsl->put_string_lf(format("   %06o", pc));

				pc += disassemble(c, cnsl, pc, true);
			}

			continue;
		}
		else if (parts[0] == "disassemble" || parts[0] == "d") {
			int pc = kv.find("pc") != kv.end() ? std::stoi(kv.find("pc")->second, nullptr, 8)  : c->getPC();
			int n  = kv.find("n")  != kv.end() ? std::stoi(kv.find("n") ->second, nullptr, 10) : 1;

			bool show_registers = kv.find("pc") == kv.end();

			for(int i=0; i<n; i++) {
				pc += disassemble(c, cnsl, pc, !show_registers);

				show_registers = false;
			}

			continue;
		}
		else if (cmd == "reset" || cmd == "r") {
			terminate = false;

			event = 0;

			c->reset();

#if defined(ESP32)
			setBootLoader(b);
#endif

			continue;
		}
#if !defined(ESP32)
		else if (cmd == "quit" || cmd == "q")
			break;
#endif
		else if (cmd == "help" || cmd == "h" || cmd == "?") {
			cnsl->put_string_lf("disassemble/d - show current instruction (pc=/n=)");
			cnsl->put_string_lf("go            - run until trap or ^e");
#if !defined(ESP32)
			cnsl->put_string_lf("quit/q        - stop emulator");
#endif
			cnsl->put_string_lf("reset/r       - reset cpu/bus/etc");
			cnsl->put_string_lf("single/s      - run 1 instruction (implicit 'disassemble' command)");
			cnsl->put_string_lf("sbp/cbp/lbp   - set/clear/list breakpoint(s)");

			continue;
		}
		else {
			cnsl->put_string_lf("?");
			continue;
		}

		c->emulation_start();

		while(!event && !*interrupt_emulation) {
			if (tracing || single_step)
				disassemble(c, single_step ? cnsl : nullptr, c->getPC(), false);

			if (c->check_breakpoint() && !single_step) {
				cnsl->put_string_lf("Breakpoint");
				break;
			}

			c->step();

			if (single_step)
				break;
		}

		if (!single_step) {
			auto speed = c->get_mips_rel_speed();
			cnsl->debug("MIPS: %.2f, relative speed: %.2f%%", speed.first, speed.second);
		}

		if (*interrupt_emulation) {
			single_step = true;

			event = 0;

			*interrupt_emulation = false;
		}
	}
}
