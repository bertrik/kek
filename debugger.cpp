// (C) 2018-2024 by Folkert van Heusden
// Released under MIT license

#include <optional>
#ifdef linux
#include <dirent.h>
#include <jansson.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#endif

#include "breakpoint_parser.h"
#include "bus.h"
#include "console.h"
#include "cpu.h"
#include "disk_backend.h"
#ifdef linux
#include "disk_backend_file.h"
#else
#include "disk_backend_esp32.h"
#endif
#include "disk_backend_nbd.h"
#include "gen.h"
#include "loaders.h"
#include "log.h"
#include "tty.h"
#include "utils.h"


#if defined(ESP32) || defined(BUILD_FOR_RP2040)
#if defined(ESP32)
#include "esp32.h"
#elif defined(BUILD_FOR_RP2040)
#include "rp2040.h"
#endif

void setBootLoader(bus *const b);

void configure_disk(console *const c);

void configure_network(console *const c);
void check_network(console *const c);
void start_network(console *const c);

void set_tty_serial_speed(console *const c, const uint32_t bps);

void recall_configuration(console *const c);
#endif

#define NET_DISK_CFG_FILE "net-disk.json"

#if !defined(BUILD_FOR_RP2040) && !defined(linux)
extern SdFs SD;
#endif

#ifndef linux
#define MAX_CFG_SIZE 1024
StaticJsonDocument<MAX_CFG_SIZE> json_doc;
#endif

typedef enum { BE_NETWORK, BE_SD } disk_backend_t;

#if !defined(BUILD_FOR_RP2040)
std::optional<std::tuple<std::vector<disk_backend *>, std::vector<disk_backend *>, std::string> > load_disk_configuration(console *const c)
{
#ifdef linux
	json_error_t error;
	json_t *json = json_load_file("." NET_DISK_CFG_FILE, JSON_REJECT_DUPLICATES, &error);
	if (!json) {
		c->put_string_lf(format("Cannot load ." NET_DISK_CFG_FILE ": %s", error.text));

		return { };
	}

	std::string nbd_host       = json_string_value (json_object_get(json, "NBD-host"));
	int         nbd_port       = json_integer_value(json_object_get(json, "NBD-port"));

	std::string disk_type_temp = json_string_value (json_object_get(json, "disk-type"));

	std::string tape_file      = json_string_value (json_object_get(json, "tape-file"));

	json_decref(json);
#else
	File dataFile = LittleFS.open("/" NET_DISK_CFG_FILE, "r");
	if (!dataFile)
		return { };

	size_t size = dataFile.size();

	char buffer[MAX_CFG_SIZE];

	if (size > sizeof buffer) {  // this should not happen
		dataFile.close();

		return { };
	}

	dataFile.read(reinterpret_cast<uint8_t *>(buffer), size);
	buffer[(sizeof buffer) - 1] = 0x00;

	dataFile.close();

	auto error = deserializeJson(json_doc, buffer);
	if (error)  // this should not happen
		return { };

	String nbd_host = json_doc["NBD-host"];
	int    nbd_port = json_doc["NBD-port"];

	String disk_type_temp = json_doc["disk-type"];

	String tape_file      = json_doc["tape-file"];
#endif

	disk_type_t disk_type = DT_RK05;

	if (disk_type_temp == "rl02")
		disk_type = DT_RL02;
	else if (disk_type_temp == "tape")
		disk_type = DT_TAPE;

	disk_backend *d = new disk_backend_nbd(nbd_host.c_str(), nbd_port);

	if (d->begin() == false) {
		c->put_string_lf("Cannot initialize NBD client from configuration file");
		delete d;
		return { };
	}

	c->put_string_lf(format("Connection to NBD server at %s:%d success", nbd_host.c_str(), nbd_port));

	if (disk_type == DT_RK05)
		return { { { d }, { }, "" } };

	if (disk_type == DT_RL02)
		return { { { }, { d }, "" } };

	if (disk_type == DT_TAPE)
		return { { { }, { }, tape_file.c_str() } };

	return { };
}

bool save_disk_configuration(const std::string & nbd_host, const int nbd_port, const std::optional<std::string> & tape_file, const disk_type_t dt, console *const cnsl)
{
#ifdef linux
	json_t *json = json_object();

	json_object_set(json, "NBD-host", json_string(nbd_host.c_str()));
	json_object_set(json, "NBD-port", json_integer(nbd_port));

	if (dt == DT_RK05)
		json_object_set(json, "disk-type", json_string("rk05"));
	else if (dt == DT_RL02)
		json_object_set(json, "disk-type", json_string("rl02"));
	else
		json_object_set(json, "disk-type", json_string("tape"));

	json_object_set(json, "tape-file", json_string(tape_file.has_value() ? tape_file.value().c_str() : ""));

	bool succeeded = json_dump_file(json, "." NET_DISK_CFG_FILE, 0) == 0;
	json_decref(json);

	if (succeeded == false) {
		cnsl->put_string_lf(format("Cannot write ." NET_DISK_CFG_FILE));

		return false;
	}
#else
	json_doc["NBD-host"] = nbd_host;
	json_doc["NBD-port"] = nbd_port;

	if (dt == DT_RK05)
		json_doc["disk-type"] = "rk05";
	else if (dt == DT_RL02)
		json_doc["disk-type"] = "rl02";
	else
		json_doc["disk-type"] = "tape";

	json_doc["tape-file"] = tape_file.has_value() ? tape_file.value() : "";

	File dataFile = LittleFS.open("/" NET_DISK_CFG_FILE, "w");
	if (!dataFile)
		return false;

	serializeJson(json_doc, dataFile);

	dataFile.close();
#endif

	return true;
}
#endif

std::optional<disk_backend_t> select_disk_backend(console *const c)
{
#if defined(BUILD_FOR_RP2040)
	return BE_SD;
#elif linux
	c->put_string("1. network (NBD), 2. local filesystem, 9. abort");
#else
	c->put_string("1. network (NBD), 2. local SD card, 9. abort");
#endif

	int ch = -1;
	while(ch == -1 && ch != '1' && ch != '2' && ch != '9') {
		auto temp = c->wait_char(500);

		if (temp.has_value())
			ch = temp.value();
	}

	c->put_string_lf(format("%c", ch));

	if (ch == '1')
		return BE_NETWORK;

	if (ch == '2')
		return BE_SD;

	return { };
}

std::optional<disk_type_t> select_disk_type(console *const c)
{
	c->put_string("1. RK05, 2. RL02, 3. tape/BIC, 9. abort");

	int ch = -1;
	while(ch == -1 && ch != '1' && ch != '2' && ch != '3' && ch != '9') {
		auto temp = c->wait_char(500);

		if (temp.has_value())
			ch = temp.value();
	}

	c->put_string_lf(format("%c", ch));

	if (ch == '1')
		return DT_RK05;

	if (ch == '2')
		return DT_RL02;

	if (ch == '3')
		return DT_TAPE;

	return { };
}

#if !defined(BUILD_FOR_RP2040)
std::optional<std::tuple<std::vector<disk_backend *>, std::vector<disk_backend *>, std::string> > select_nbd_server(console *const c)
{
	c->flush_input();

	std::string hostname = c->read_line("Enter hostname (or empty to abort): ");

	if (hostname.empty())
		return { };

	std::string port_str = c->read_line("Enter port number (or empty to abort): ");

	if (port_str.empty())
		return { };

	auto disk_type = select_disk_type(c);

	if (disk_type.has_value() == false)
		return { };

	disk_backend *d = new disk_backend_nbd(hostname, atoi(port_str.c_str()));

	if (d->begin() == false) {
		c->put_string_lf("Cannot initialize NBD client");
		delete d;
		return { };
	}

	if (save_disk_configuration(hostname, atoi(port_str.c_str()), { }, disk_type.value(), c))
		c->put_string_lf("NBD disk configuration saved");
	else
		c->put_string_lf("NBD disk configuration NOT saved");

	if (disk_type.value() == DT_RK05)
		return { { { d }, { }, "" } };

	if (disk_type.value() == DT_RL02)
		return { { { }, { d }, "" } };

	return { };
}
#endif

// RK05, RL02 files
std::optional<std::tuple<std::vector<disk_backend *>, std::vector<disk_backend *>, std::string> > select_disk_files(console *const c)
{
#ifdef linux
	c->put_string_lf("Files in current directory: ");
#else
	c->debug("MISO: %d", int(MISO));
	c->debug("MOSI: %d", int(MOSI));
	c->debug("SCK : %d", int(SCK ));
	c->debug("SS  : %d", int(SS  ));

	c->put_string_lf("Files on SD-card:");

#if defined(SHA2017)
	if (!SD.begin(21, SD_SCK_MHZ(10)))
		SD.initErrorHalt();
#elif !defined(BUILD_FOR_RP2040)
	if (!SD.begin(SS, SD_SCK_MHZ(15))) {
		auto err = SD.sdErrorCode();
		if (err)
			c->debug("SDerror: 0x%x, data: 0x%x", err, SD.sdErrorData());
		else
			c->debug("Failed to initialize SD card");

		return { };
	}
#endif
#endif

	for(;;) {
#if defined(linux)
		DIR *dir = opendir(".");
		if (!dir) {
			c->put_string_lf("Cannot access directory");
			return { };
		}

		dirent *dr = nullptr;
		while((dr = readdir(dir))) {
			struct stat st { };

			if (stat(dr->d_name, &st) == 0)
				c->put_string_lf(format("%s\t\t%ld", dr->d_name, st.st_size));
		}

		closedir(dir);
#elif defined(BUILD_FOR_RP2040)
		File root = SD.open("/");

		for(;;) {
			auto entry = root.openNextFile();
			if (!entry)
				break;

			if (!entry.isDirectory()) {
				c->put_string(entry.name());
				c->put_string("\t\t");
				c->put_string_lf(format("%ld", entry.size()));
			}

			entry.close();
		}
#else
		SD.ls("/", LS_DATE | LS_SIZE | LS_R);
#endif

		c->flush_input();

		std::string selected_file = c->read_line("Enter filename (or empty to abort): ");

		if (selected_file.empty())
			return { };

		auto disk_type = select_disk_type(c);

		if (disk_type.has_value() == false)
			return { };

		c->put_string("Opening file: ");
		c->put_string_lf(selected_file.c_str());

		bool can_open_file = false;

#ifdef linux
		struct stat st { };
		can_open_file = stat(selected_file.c_str(), &st) == 0;
#else
		File32 fh;
		can_open_file = fh.open(selected_file.c_str(), O_RDWR);
		if (can_open_file)
			fh.close();
#endif

		if (can_open_file) {
			if (disk_type.value() == DT_TAPE)
				return { { { }, { }, selected_file } };

#ifdef linux
			disk_backend *temp = new disk_backend_file(selected_file);
#else
			disk_backend *temp = new disk_backend_esp32(selected_file);
#endif

			if (!temp->begin()) {
				c->put_string("Cannot use: ");
				c->put_string_lf(selected_file.c_str());

				delete temp;

				continue;
			}

			if (disk_type.value() == DT_RK05)
				return { { { temp }, { }, "" } };

			if (disk_type.value() == DT_RL02)
				return { { { }, { temp }, "" } };
		}

		c->put_string_lf("open failed");
	}
}

void set_disk_configuration(bus *const b, console *const cnsl, std::tuple<std::vector<disk_backend *>, std::vector<disk_backend *>, std::string> & disk_files)
{
	if (std::get<0>(disk_files).empty() == false)
		b->add_rk05(new rk05(std::get<0>(disk_files), b, cnsl->get_disk_read_activity_flag(), cnsl->get_disk_write_activity_flag()));

	if (std::get<1>(disk_files).empty() == false)
		b->add_rl02(new rl02(std::get<1>(disk_files), b, cnsl->get_disk_read_activity_flag(), cnsl->get_disk_write_activity_flag()));

	if (std::get<2>(disk_files).empty() == false) {
		auto addr = loadTape(b, std::get<2>(disk_files));

		if (addr.has_value())
			b->getCpu()->setPC(addr.value());
	}

	if (std::get<0>(disk_files).empty() == false)
		setBootLoader(b, BL_RK05);
	else if (std::get<1>(disk_files).empty() == false)
		setBootLoader(b, BL_RL02);
}

void configure_disk(bus *const b, console *const cnsl)
{
	for(;;) {
		cnsl->put_string_lf("Load disk");

		auto backend = select_disk_backend(cnsl);

		if (backend.has_value() == false)
			break;

		std::optional<std::tuple<std::vector<disk_backend *>, std::vector<disk_backend *>, std::string> > files;

#if !defined(BUILD_FOR_RP2040)
		if (backend == BE_NETWORK)
			files = select_nbd_server(cnsl);
		else // if (backend == BE_SD)
#endif
			files = select_disk_files(cnsl);

		if (files.has_value() == false)
			break;

		set_disk_configuration(b, cnsl, files.value());

		break;
	}
}

// returns size of instruction (in bytes)
int disassemble(cpu *const c, console *const cnsl, const uint16_t pc, const bool instruction_only)
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

	std::string MMR0 = data["MMR0"].at(0);
	std::string MMR1 = data["MMR1"].at(0);
	std::string MMR2 = data["MMR2"].at(0);
	std::string MMR3 = data["MMR3"].at(0);

	std::string result;

	if (instruction_only)
		result = format("PC: %06o, instr: %s\t%s\t%s",
				pc,
				instruction_values.c_str(),
				instruction.c_str(),
				work_values.c_str()
				);
	else
		result = format("R0: %s, R1: %s, R2: %s, R3: %s, R4: %s, R5: %s, SP: %s, PC: %06o, PSW: %s (%s), instr: %s: %s",
				registers[0].c_str(), registers[1].c_str(), registers[2].c_str(), registers[3].c_str(), registers[4].c_str(), registers[5].c_str(),
				registers[6].c_str(), pc, 
				psw.c_str(), data["psw-value"][0].c_str(),
				instruction_values.c_str(),
				instruction.c_str()
				);

	if (cnsl)
		cnsl->debug(result);
	else
		DOLOG(debug, false, "%s", result.c_str());

	std::string sp;
	for(auto sp_val : data["sp"])
		sp += (sp.empty() ? "" : ",") + sp_val;

	DOLOG(debug, false, "SP: %s, MMR0/1/2/3: %s/%s/%s/%s", sp.c_str(), MMR0.c_str(), MMR1.c_str(), MMR2.c_str(), MMR3.c_str());

#if 0
	if (c->getPSW_runmode() == 3) {
		/*
		FILE *fh = fopen("/home/folkert/temp/ramdisk/log-kek.dat", "a+");
		fprintf(fh, "%06o", pc);
		for(auto & v: data["instruction-values"])
			fprintf(fh, " %s", v.c_str());
		fprintf(fh, "\n");
		fclose(fh);
		*/
		FILE *fh = fopen("/home/folkert/temp/ramdisk/da-kek.txt", "a+");
		fprintf(fh, "R0 %s R1 %s R2 %s R3 %s R4 %s R5 %s R6 %s R7 %06o %s\n", registers[0].c_str(), registers[1].c_str(), registers[2].c_str(), registers[3].c_str(), registers[4].c_str(), registers[5].c_str(), registers[6].c_str(), pc, instruction.c_str());
		fclose(fh);
	}
#endif

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

void dump_par_pdr(console *const cnsl, bus *const b, const uint16_t pdrs, const uint16_t pars, const std::string & name, const int state, const std::optional<int> & selection)
{
	if (state == 0 || state == 2)
		cnsl->put_string_lf(name);
	else
		cnsl->put_string_lf(format("%s DISABLED", name.c_str()));

	cnsl->put_string_lf("   PAR             PDR    LEN");

	for(int i=0; i<8; i++) {
		if (selection.has_value() && i != selection.value())
			continue;
		uint16_t par_value = b->read(pars + i * 2, wm_word, rm_cur, true);
		uint16_t pdr_value = b->read(pdrs + i * 2, wm_word, rm_cur, true);

		uint16_t pdr_len   = (((pdr_value >> 8) & 127) + 1) * 64;

		cnsl->put_string_lf(format("%d] %06o %08o %06o %04o D%d A%d", i, par_value, par_value * 64, pdr_value, pdr_len, !!(pdr_value & 8), pdr_value & 7));
	}
}

void dump_memory_contents(console *const cnsl, bus *const b, const uint16_t read_addr)
{
	cnsl->put_string_lf(format("\tMOV #%06o,R0", read_addr));
	cnsl->put_string_lf(format("\tMOV #%06o,(R0)", b->read(read_addr, wm_word, rm_cur, true)));
}

void dump_range_as_instructions(console *const cnsl, bus *const b, const uint16_t base)
{
	for(int i=0; i<8; i++)
		dump_memory_contents(cnsl, b, base + i * 2);
}

void mmu_dump(console *const cnsl, bus *const b, const bool verbose)
{
	uint16_t mmr0 = b->getMMU()->getMMR0();
	uint16_t mmr1 = b->getMMU()->getMMR1();
	uint16_t mmr2 = b->getMMU()->getMMR2();
	uint16_t mmr3 = b->getMMU()->getMMR3();

	cnsl->put_string_lf(mmr0 & 1 ? "MMU enabled" : "MMU NOT enabled");

	cnsl->put_string_lf(format("MMR0: %06o", mmr0));
	cnsl->put_string_lf(format("MMR1: %06o", mmr1));
	cnsl->put_string_lf(format("MMR2: %06o", mmr2));
	cnsl->put_string_lf(format("MMR3: %06o", mmr3));

	dump_par_pdr(cnsl, b, ADDR_PDR_SV_START,       ADDR_PAR_SV_START,       "supervisor i-space", 0, { });
	dump_par_pdr(cnsl, b, ADDR_PDR_SV_START + 020, ADDR_PAR_SV_START + 020, "supervisor d-space", 1 + (!!(mmr3 & 2)), { });

	dump_par_pdr(cnsl, b, ADDR_PDR_K_START,       ADDR_PAR_K_START,       "kernel i-space", 0, { });
	dump_par_pdr(cnsl, b, ADDR_PDR_K_START + 020, ADDR_PAR_K_START + 020, "kernel d-space", 1 + (!!(mmr3 & 4)), { });

	dump_par_pdr(cnsl, b, ADDR_PDR_U_START,       ADDR_PAR_U_START,       "user i-space", 0, { });
	dump_par_pdr(cnsl, b, ADDR_PDR_U_START + 020, ADDR_PAR_U_START + 020, "user d-space", 1 + (!!(mmr3 & 1)), { });

	if (verbose) {
		dump_range_as_instructions(cnsl, b, ADDR_PDR_SV_START);  // sv i
		dump_range_as_instructions(cnsl, b, ADDR_PDR_SV_START + 020);  // sv d
		dump_range_as_instructions(cnsl, b, ADDR_PDR_K_START);  // k i
		dump_range_as_instructions(cnsl, b, ADDR_PDR_K_START + 020);  // k d
		dump_range_as_instructions(cnsl, b, ADDR_PDR_U_START);  // u i
		dump_range_as_instructions(cnsl, b, ADDR_PDR_U_START + 020);  // u d

		dump_memory_contents(cnsl, b, ADDR_MMR0);
		dump_memory_contents(cnsl, b, ADDR_MMR1);
		dump_memory_contents(cnsl, b, ADDR_MMR2);
		dump_memory_contents(cnsl, b, ADDR_MMR3);
	}
}

const char *trap_action_to_str(const trap_action_t ta)
{
	if (ta == T_PROCEED)
		return "proceed";
	if (ta == T_ABORT_4)
		return "abort (trap 4)";
	if (ta == T_TRAP_250)
		return "trap 250";

	return "?";
}

void mmu_resolve(console *const cnsl, bus *const b, const uint16_t va)
{
	int  run_mode = b->getCpu()->getPSW_runmode();
	cnsl->put_string_lf(format("Run mode: %d, use data space: %d", run_mode, b->getMMU()->get_use_data_space(run_mode)));

	auto data     = b->calculate_physical_address(run_mode, va);

	uint16_t page_offset = va & 8191;
	cnsl->put_string_lf(format("Active page field: %d, page offset: %o (%d)", data.apf, page_offset, page_offset));
	cnsl->put_string_lf(format("Phys. addr. instruction: %08o (psw: %d)", data.physical_instruction, data.physical_instruction_is_psw));
	cnsl->put_string_lf(format("Phys. addr. data: %08o (psw: %d)", data.physical_data, data.physical_data_is_psw));

	uint16_t mmr3 = b->getMMU()->getMMR3();

	if (run_mode == 0) {
		dump_par_pdr(cnsl, b, ADDR_PDR_K_START,       ADDR_PAR_K_START,       "kernel i-space", 0, data.apf);
		dump_par_pdr(cnsl, b, ADDR_PDR_K_START + 020, ADDR_PAR_K_START + 020, "kernel d-space", 1 + (!!(mmr3 & 4)), data.apf);
	}
	else if (run_mode == 1) {
		dump_par_pdr(cnsl, b, ADDR_PDR_SV_START,       ADDR_PAR_SV_START,       "supervisor i-space", 0, data.apf);
		dump_par_pdr(cnsl, b, ADDR_PDR_SV_START + 020, ADDR_PAR_SV_START + 020, "supervisor d-space", 1 + (!!(mmr3 & 4)), data.apf);
	}
	else if (run_mode == 3) {
		dump_par_pdr(cnsl, b, ADDR_PDR_U_START,       ADDR_PAR_U_START,       "user i-space", 0, data.apf);
		dump_par_pdr(cnsl, b, ADDR_PDR_U_START + 020, ADDR_PAR_U_START + 020, "user d-space", 1 + (!!(mmr3 & 4)), data.apf);
	}

	for(int i=0; i<2; i++) {
		auto ta_i = b->get_trap_action(run_mode, false, data.apf, i);
		auto ta_d = b->get_trap_action(run_mode, true,  data.apf, i);

		cnsl->put_string_lf(format("Instruction action: %s (%s)", trap_action_to_str(ta_i.first), i ? "write" : "read"));
		cnsl->put_string_lf(format("Data action       : %s (%s)", trap_action_to_str(ta_d.first), i ? "write" : "read"));
	}
}

void reg_dump(console *const cnsl, cpu *const c)
{
	for(uint8_t set=0; set<2; set++) {
		cnsl->put_string_lf(format("Set %d, R0: %06o, R1: %06o, R2: %06o, R3: %06o, R4: %06o, R5: %06o",
						set,
						c->lowlevel_register_get(set, 0),
						c->lowlevel_register_get(set, 1),
						c->lowlevel_register_get(set, 2),
						c->lowlevel_register_get(set, 3),
						c->lowlevel_register_get(set, 4),
						c->lowlevel_register_get(set, 5)));
	}

	cnsl->put_string_lf(format("PSW: %06o, PC: %06o, run mode: %d", c->getPSW(), c->lowlevel_register_get(0, 7), c->getPSW_runmode()));

	cnsl->put_string_lf(format("STACK: k:%06o, sv:%06o, -:%06o, usr: %06o",
				c->lowlevel_register_sp_get(0),
				c->lowlevel_register_sp_get(1),
				c->lowlevel_register_sp_get(2),
				c->lowlevel_register_sp_get(3)));
}

void show_run_statistics(console *const cnsl, cpu *const c)
{
	auto stats = c->get_mips_rel_speed({ }, { });

	cnsl->put_string_lf(format("Executed %zu instructions in %.2f ms of which %.2f ms idle", size_t(std::get<2>(stats)), std::get<3>(stats) / 1000., std::get<4>(stats) / 1000.));
	cnsl->put_string_lf(format("MIPS: %.2f, relative speed: %.2f%%", std::get<0>(stats), std::get<1>(stats)));
}

void show_queued_interrupts(console *const cnsl, cpu *const c)
{
	cnsl->put_string_lf(format("Current level: %d", c->getPSW_spl()));

	auto delay = c->get_interrupt_delay_left();
	if (delay.has_value())
		cnsl->put_string_lf(format("Current delay left: %d", delay.value()));
	else
		cnsl->put_string_lf("No delay");

	cnsl->put_string_lf(format("Interrupt pending flag: %d", c->check_if_interrupts_pending()));

	auto queued_interrupts = c->get_queued_interrupts();

	for(auto & level: queued_interrupts) {
		for(auto & qi: level.second)
			cnsl->put_string_lf(format("Level: %d, interrupt: %03o", level.first, qi));
	}
}

void debugger(console *const cnsl, bus *const b, std::atomic_uint32_t *const stop_event, const bool tracing_in)
{
	int32_t trace_start_addr = -1;
	bool    tracing          = tracing_in;
	int     n_single_step    = 1;
	bool    turbo            = false;
	std::optional<int> t_rl;  // trace runlevel

	cpu *const c = b->getCpu();

	b->set_debug_mode();

	bool single_step = false;

	while(*stop_event != EVENT_TERMINATE) {
		try {
			std::string cmd   = cnsl->read_line(format("%d", stop_event->load()));
			auto        parts = split(cmd, " ");
			auto        kv    = split(parts, "=");

			if (parts.empty())
				continue;

			if (cmd == "go") {
				single_step = false;

				*stop_event = EVENT_NONE;
			}
			else if (parts[0] == "single" || parts[0] == "s") {
				single_step = true;

				if (parts.size() == 2)
					n_single_step = atoi(parts[1].c_str());
				else
					n_single_step = 1;

				*stop_event = EVENT_NONE;
			}
			else if ((parts[0] == "sbp" || parts[0] == "cbp") && parts.size() >= 2){
				if (parts[0] == "sbp") {
					std::size_t space = cmd.find(" ");

					std::pair<breakpoint *, std::optional<std::string> > rc = parse_breakpoint(b, cmd.substr(space + 1));

					if (rc.first == nullptr) {
						if (rc.second.has_value())
							cnsl->put_string_lf(rc.second.value());
						else
							cnsl->put_string_lf("not set");
					}
					else {
						int id = c->set_breakpoint(rc.first);

						cnsl->put_string_lf(format("Breakpoint has id: %d", id));
					}
				}
				else {
					if (c->remove_breakpoint(std::stoi(parts[1])))
						cnsl->put_string_lf("Breakpoint cleared");
					else
						cnsl->put_string_lf("Breakpoint not found");
				}

				continue;
			}
			else if (cmd == "lbp") {
				auto bps = c->list_breakpoints();

				cnsl->put_string_lf("Breakpoints:");

				for(auto a : bps)
					cnsl->put_string_lf(format("%d: %s", a.first, a.second->emit().c_str()));

				if (bps.empty())
					cnsl->put_string_lf("(none)");

				continue;
			}
			else if (parts[0] == "disassemble" || parts[0] == "d") {
				uint16_t pc = kv.find("pc") != kv.end() ? std::stoi(kv.find("pc")->second, nullptr, 8)  : c->getPC();
				int n  = kv.find("n")  != kv.end() ? std::stoi(kv.find("n") ->second, nullptr, 10) : 1;

				cnsl->put_string_lf(format("Disassemble %d instructions starting at %o", n, pc));

				bool show_registers = kv.find("pc") == kv.end();

				for(int i=0; i<n; i++) {
					pc += disassemble(c, cnsl, pc, !show_registers);

					show_registers = false;
				}

				continue;
			}
			else if (parts[0] == "setpc") {
				if (parts.size() == 2) {
					uint16_t new_pc = std::stoi(parts.at(1), nullptr, 8);
					c->setPC(new_pc);

					cnsl->put_string_lf(format("Set PC to %06o", new_pc));
				}
				else {
					cnsl->put_string_lf("setpc requires an (octal address as) parameter");
				}

				continue;
			}
			else if (parts[0] == "toggle") {
				auto s_it = kv.find("s");
				auto t_it = kv.find("t");

				if (s_it == kv.end() || t_it == kv.end())
					cnsl->put_string_lf(format("toggle: parameter missing? current switches states: 0o%06o", c->getBus()->get_console_switches()));
				else {
					int s = std::stoi(s_it->second, nullptr, 8);
					int t = std::stoi(t_it->second, nullptr, 8);

					c->getBus()->set_console_switch(s, t);

					cnsl->put_string_lf(format("Set switch %d to %d", s, t));
				}

				continue;
			}
			else if (parts[0] == "setmem") {
				auto a_it = kv.find("a");
				auto v_it = kv.find("v");

				if (a_it == kv.end() || v_it == kv.end())
					cnsl->put_string_lf("setmem: parameter missing?");
				else {
					uint16_t a = std::stoi(a_it->second, nullptr, 8);
					uint8_t  v = std::stoi(v_it->second, nullptr, 8);

					c->getBus()->writeByte(a, v);

					cnsl->put_string_lf(format("Set %06o to %03o", a, v));
				}

				continue;
			}
			else if (parts[0] == "trace" || parts[0] == "t") {
				tracing = !tracing;

				cnsl->put_string_lf(format("Tracing set to %s", tracing ? "ON" : "OFF"));

				continue;
			}
			else if (parts[0] == "mmudump") {
				mmu_dump(cnsl, b, parts.size() == 2 && parts[1] == "-v");

				continue;
			}
			else if (parts[0] == "mmures") {
				if (parts.size() == 2)
					mmu_resolve(cnsl, b, std::stoi(parts[1], nullptr, 8));
				else
					cnsl->put_string_lf("Parameter missing");

				continue;
			}
			else if (parts[0] == "regdump") {
				reg_dump(cnsl, c);

				continue;
			}
			else if (parts[0] == "strace") {
				if (parts.size() != 2) {
					trace_start_addr = -1;

					cnsl->put_string_lf("Tracing start address reset");
				}
				else {
					trace_start_addr = std::stoi(parts[1], nullptr, 8);

					cnsl->put_string_lf(format("Tracing start address set to %06o", trace_start_addr));
				}

				continue;
			}
			else if (parts[0] == "examine" || parts[0] == "e") {
				if (parts.size() < 3)
					cnsl->put_string_lf("parameter missing");
				else {
					uint32_t addr = std::stoi(parts[1], nullptr, 8);
					int      n    = parts.size() == 4 ? atoi(parts[3].c_str()) : 1;

					if (parts[2] != "p" && parts[2] != "v") {
						cnsl->put_string_lf("expected p (physical address) or v (virtual address)");

						continue;
					}

					std::string out;

					for(int i=0; i<n; i++) {
						uint32_t cur_addr = addr + i * 2;
						int val = parts[2] == "v" ? b->read(cur_addr, wm_word, rm_cur, true) : b->readPhysical(cur_addr);

						if (val == -1) {
							cnsl->put_string_lf(format("Can't read from %06o\n", cur_addr));
							break;
						}

						if (n == 1)
							cnsl->put_string_lf(format("value at %06o, octal: %o, hex: %x, dec: %d\n", cur_addr, val, val, val));

						if (n > 1) {
							if (i > 0)
								out += " ";

							out += format("%06o=%06o", cur_addr, val);
						}
					}

					if (n > 1)
						cnsl->put_string_lf(out);
				}

				continue;
			}
			else if (cmd == "reset" || cmd == "r") {
				*stop_event = EVENT_NONE;
				b->reset();
				continue;
			}
			else if (cmd == "cfgdisk") {
				configure_disk(b, cnsl);

				continue;
			}
#if defined(ESP32)
			else if (cmd == "cfgnet") {
				configure_network(cnsl);

				continue;
			}
			else if (cmd == "chknet") {
				check_network(cnsl);

				continue;
			}
			else if (cmd == "startnet") {
				start_network(cnsl);

				continue;
			}
			else if (parts.at(0) == "serspd") {
				if (parts.size() == 2) {
					uint32_t speed = std::stoi(parts.at(1), nullptr, 10);
					set_tty_serial_speed(cnsl, speed);

					cnsl->put_string_lf(format("Set baudrate to %d", speed));
				}
				else {
					cnsl->put_string_lf("serspd requires an (decimal) parameter");
				}

				continue;
			}
			else if (cmd == "init") {
				recall_configuration(cnsl);

				continue;
			}
#endif
			else if (cmd == "stats") {
				show_run_statistics(cnsl, c);

				continue;
			}
			else if (parts[0] == "ramsize") {
				if (parts.size() == 2)
					b->set_memory_size(std::stoi(parts.at(1)));
				else {
					int n_pages = b->get_memory_size();

					cnsl->put_string_lf(format("Memory size: %u pages or %u kB (decimal)", n_pages, n_pages * 8192 / 1024));
				}

				continue;
			}
			else if (parts[0] == "bl" && parts.size() == 2) {
				setBootLoader(b, parts.at(1) == "rk05" ? BL_RK05 : BL_RL02);
				cnsl->put_string_lf("Bootloader set");

				continue;
			}
			else if (parts[0] == "trl") {
				if (parts.size() == 1)
					t_rl.reset();
				else
					t_rl = std::stoi(parts.at(1));

				continue;
			}
			else if (cmd == "cls") {
				const char cls[] = { 27, '[', '2', 'J', 12, 0 };

				cnsl->put_string_lf(cls);

				continue;
			}
			else if (cmd == "turbo") {
				turbo = !turbo;

				if (turbo)
					c->set_debug(false);

				cnsl->put_string_lf(format("Turbo set to %s", turbo ? "ON" : "OFF"));

				continue;
			}
			else if (cmd == "debug") {
				bool new_mode = !c->get_debug();
				c->set_debug(new_mode);

				cnsl->put_string_lf(format("Debug mode set to %s", new_mode ? "ON" : "OFF"));

				continue;
			}
			else if (parts[0] == "setll" && parts.size() == 2) {
				auto ll_parts = split(parts[1], ",");

				if (ll_parts.size() != 2)
					cnsl->put_string_lf("Loglevel for either screen or file missing");
				else {
					log_level_t ll_screen  = parse_ll(ll_parts[0]);
					log_level_t ll_file    = parse_ll(ll_parts[1]);

					setll(ll_screen, ll_file);
				}
			}
			else if (parts[0] == "setll" && parts.size() == 2) {
				auto ll_parts = split(parts[1], ",");

				if (ll_parts.size() != 2)
					cnsl->put_string_lf("Loglevel for either screen or file missing");
				else {
					log_level_t ll_screen  = parse_ll(ll_parts[0]);
					log_level_t ll_file    = parse_ll(ll_parts[1]);

					setll(ll_screen, ll_file);
				}

				continue;
			}
			else if (parts[0] == "setsl" && parts.size() == 3) {
				setloghost(parts.at(1).c_str(), parse_ll(parts[2]));

				continue;
			}
			else if (cmd == "qi") {
				show_queued_interrupts(cnsl, c);

				continue;
			}
			else if (cmd == "bt") {
				if (c->get_debug() == false)
					cnsl->put_string_lf("Debug mode is disabled!");

				auto backtrace = c->get_stack_trace();

				for(auto & element: backtrace)
					cnsl->put_string_lf(format("%06o %s", element.first, element.second.c_str()));

				continue;
			}
			else if (cmd == "quit" || cmd == "q") {
#if defined(ESP32)
				ESP.restart();
#endif
				break;
			}
			else if (cmd == "help" || cmd == "h" || cmd == "?") {
				constexpr const char *const help[] = {
					"disassemble/d - show current instruction (pc=/n=)",
					"go            - run until trap or ^e",
#if !defined(ESP32)
					"quit/q        - stop emulator",
#endif
					"examine/e     - show memory address (<octal address> <p|v> [<n>])",
					"reset/r       - reset cpu/bus/etc",
					"single/s      - run 1 instruction (implicit 'disassemble' command)",
					"sbp/cbp/lbp   - set/clear/list breakpoint(s)",
					"                e.g.: (pc=0123 and memwv[04000]=0200,0300 and (r4=07,05 or r5=0456))",
					"                values seperated by ',', char after mem is w/b (word/byte), then",
					"                follows v/p (virtual/physical), all octal values, mmr0-3 and psw are",
					"                registers",
					"trace/t       - toggle tracing",
					"setll         - set loglevel: terminal,file",
					"setsl         - set syslog target: requires a hostname and a loglevel",
					"turbo         - toggle turbo mode (cannot be interrupted)",
					"debug         - enable CPU debug mode",
					"bt            - show backtrace - need to enable debug first",
					"strace        - start tracing from address - invoke without address to disable",
					"trl           - set trace run-level, empty for all",
					"regdump       - dump register contents",
					"mmudump       - dump MMU settings (PARs/PDRs)",
					"mmures        - resolve a virtual address",
					"qi            - show queued interrupts",
					"setpc         - set PC to value",
					"setmem        - set memory (a=) to value (v=), both in octal, one byte",
					"toggle        - set switch (s=, 0...15 (decimal)) of the front panel to state (t=, 0 or 1)",
					"cls           - clear screen",
					"stats         - show run statistics",
					"ramsize       - set ram size (page count (8 kB))",
					"bl            - set bootload (rl02 or rk05)",
#if defined(ESP32)
					"cfgnet        - configure network (e.g. WiFi)",
					"startnet      - start network",
					"chknet        - check network status",
					"serspd        - set serial speed in bps (8N1 are default)",
					"init          - reload (disk-)configuration from flash",
#endif
					"cfgdisk       - configure disk",
					nullptr
				};

				size_t i=0;
				while(help[i])
					cnsl->put_string_lf(help[i++]);
				continue;
			}
			else {
				cnsl->put_string_lf("?");
				continue;
			}

			c->emulation_start();

			*cnsl->get_running_flag() = true;

			bool reset_cpu = true;

			if (turbo) {
				while(*stop_event == EVENT_NONE)
					c->step();
			}
			else {
				reset_cpu = false;

				while(*stop_event == EVENT_NONE) {
					if (!single_step)
						DOLOG(debug, false, "---");

					if (trace_start_addr != -1 && c->getPC() == trace_start_addr)
						tracing = true;

					if ((tracing || single_step) && (t_rl.has_value() == false || t_rl.value() == c->getPSW_runmode()))
						disassemble(c, single_step ? cnsl : nullptr, c->getPC(), false);

					auto bp_result = c->check_breakpoint();
					if (bp_result.has_value() && !single_step) {
						cnsl->put_string_lf("Breakpoint: " + bp_result.value());
						break;
					}

					c->step();

					if (single_step && --n_single_step == 0)
						break;
				}
			}

			*cnsl->get_running_flag() = false;

			if (reset_cpu)
				c->reset();
		}
		catch(const std::exception & e) {
			cnsl->put_string_lf(format("Exception caught: %s", e.what()));
		}
		catch(const int ei) {
			cnsl->put_string_lf(format("Problem: %d", ei));
		}
		catch(...) {
			cnsl->put_string_lf("Unspecified exception caught");
		}
	}
}

void run_bic(console *const cnsl, bus *const b, std::atomic_uint32_t *const stop_event, const bool tracing, const uint16_t start_addr)
{
	cpu *const c = b->getCpu();

	c->setRegister(7, start_addr);

	*cnsl->get_running_flag() = true;

	while(*stop_event == EVENT_NONE) {
		if (tracing)
			disassemble(c, nullptr, c->getPC(), false);

		c->step();
	}

	*cnsl->get_running_flag() = false;
}
