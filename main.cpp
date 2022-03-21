// (C) 2018-2022 by Folkert van Heusden
// Released under Apache License v2.0
#include <atomic>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "memory.h"
#include "cpu.h"
#include "tty.h"
#include "utils.h"
#include "tests.h"
#include "terminal.h"
#include "error.h"

struct termios   org_tty_opts { 0 };
bool             withUI       { false };
uint32_t         event        { 0 };
std::atomic_bool terminate    { false };


void reset_terminal()
{
	if (withUI)
		endwin();
	else
		tcsetattr(STDIN_FILENO, TCSANOW, &org_tty_opts);
}

void loadbin(bus *const b, uint16_t base, const char *const file)
{
	FILE *fh = fopen(file, "rb");

	while(!feof(fh))
		b -> writeByte(base++, fgetc(fh));

	fclose(fh);
}

void setBootLoader(bus *const b)
{
	cpu *const c = b -> getCpu();

	const uint16_t offset = 01000;
	constexpr uint16_t bootrom[] = {
		0012700,
		0177406,
		0012710,
		0177400,
		0012740,
		0000005,
		0105710,
		0100376,
		0005007
	};

	FILE *fh = fopen("boot.dat", "wb");

	for(size_t i=0; i<sizeof bootrom / 2; i++) {
		b -> writeWord(offset + i * 2, bootrom[i]);
		fputc(bootrom[i] & 255, fh);
		fputc(bootrom[i] >> 8, fh);
	}

	fclose(fh);

	c -> setRegister(7, offset);
}

uint16_t loadTape(bus *const b, const char *const file)
{
	FILE *fh = fopen(file, "rb");
	if (!fh) {
		fprintf(stderr, "Cannot open %s\n", file);
		return -1;
	}

	uint16_t start = 0, end = 0;

	for(;!feof(fh);) {
		uint8_t buffer[6];

		if (fread(buffer, 1, 6, fh) != 6)
			break;

		int count = (buffer[3] << 8) | buffer[2];
		int p = (buffer[5] << 8) | buffer[4];

		uint8_t csum = 0;
		for(int i=2; i<6; i++)
			csum += buffer[i];

		if (count == 6) { // eg no data
			if (p != 1) {
				fprintf(stderr, "Setting start address to %o\n", p);
				start = p;
			}
		}

		fprintf(stderr, "%ld] reading %d (dec) bytes to %o (oct)\n", ftell(fh), count - 6, p);

		for(int i=0; i<count - 6; i++) {
			if (feof(fh)) {
				fprintf(stderr, "short read\n");
				break;
			}
			uint8_t c = fgetc(fh);

			csum += c;
			b -> writeByte(p++, c);

			if (p > end)
				end = p;
		}

		int fcs = fgetc(fh);
		csum += fcs;

		if (csum != 255)
			fprintf(stderr, "checksum error %d\n", csum);
	}

	fclose(fh);

	fh = fopen("test.dat", "wb");
	for(int i=0; i<end; i++)
		fputc(b -> readByte(i), fh);
	fclose(fh);

	return start;
}

NEWWIN *w_main_b = nullptr, *w_main = nullptr;

void resize_terminal()
{
	determine_terminal_size();

	if (ERR == resizeterm(max_y, max_x))
		error_exit(true, "problem resizing terminal");

	wresize(stdscr, max_y, max_x);

	endwin();
	refresh();

	wclear(stdscr);

	delete_window(w_main_b);
	delete_window(w_main);
	create_win_border(0, 0, max_x - 2, max_y - 2, "window", &w_main_b, &w_main, false);
	scrollok(w_main -> win, TRUE);

	mydoupdate();
}

volatile bool sw = false;
void sw_handler(int s)
{
	if (s == SIGWINCH)
		sw = true;
	else {
		fprintf(stderr, "Terminating...\n");

		terminate = true;
	}
}

bool poll_char()
{
	struct pollfd fds[] = { { STDIN_FILENO, POLLIN, 0 } };

	return poll(fds, 1, 0) == 1 && fds[0].revents;
}

char get_char()
{
	char c = getchar();

	if (c == 3)
		event = 1;

	return c;
}

char get_char_ui()
{
	char c = getch();

	if (c == 3)
		event = 1;

	return c;
}

void put_char(char c)
{
	printf("%c", c);
	fflush(nullptr);
}

void put_char_ui(char c)
{
	if (c >= 32 || (c != 12 && c != 27 && c != 13)) {
		wprintw(w_main -> win, "%c", c);
		mydoupdate();
	}
}

void help()
{
	printf("-h       this help\n");
	printf("-m mode  \"tc\": run testcases\n");
	printf("-T t.bin load file as a binary tape file (like simh \"load\" command)\n");
	printf("-R d.rk  load file as a RK05 disk device\n");
	printf("-p 123   set CPU start pointer to decimal(!) value\n");
	printf("-L f.bin load file into memory at address given by -p (and run it)\n");
	printf("-n       ncurses UI\n");
	printf("-d       enable disassemble\n");
}

int main(int argc, char *argv[])
{
	//setlocale(LC_ALL, "");

	fprintf(stderr, "This PDP-11 emulator is called \"kek\" (reason for that is forgotten) and was written by Folkert van Heusden.\n");

	fprintf(stderr, "Build on: " __DATE__ " " __TIME__ "\n");

	bus *b = new bus();
	cpu *c = new cpu(b, &event);
	b->add_cpu(c);

	c -> setEmulateMFPT(true);

	bool testCases = false;
	int opt = -1;
	while((opt = getopt(argc, argv, "hm:T:R:p:ndL:")) != -1)
	{
		switch(opt) {
			case 'h':
				help();
				return 1;

			case 'd':
				c->setDisassemble(true);
				break;

			case 'n':
				withUI = true;
				break;

			case 'm':
				if (strcasecmp(optarg, "tc") == 0)
					testCases = true;
				else {
					fprintf(stderr, "\"-m %s\" is not known\n", optarg);
					return 1;
				}
				break;

			case 'T':
				c->setRegister(7, loadTape(b, optarg));
				break;

			case 'R': {
					  b->add_rk05(new rk05(optarg, b, nullptr, nullptr));
					  setBootLoader(b);
					  break;
				  }

			case 'p':
				c->setRegister(7, atoi(optarg));
				break;

			case 'L':
				loadbin(b, c->getRegister(7), optarg);
				break;

			default:
				  fprintf(stderr, "-%c is not understood\n", opt);
				  return 1;
		}
	}

	tty *tty_ = nullptr;

	if (withUI)
		tty_ = new tty(poll_char, get_char_ui, put_char_ui);
	else
		tty_ = new tty(poll_char, get_char, put_char);

	b->add_tty(tty_);

	if (testCases)
		tests(c);

	fprintf(stderr, "Start running at %o\n", c->getRegister(7));

	struct sigaction sa { };
	sa.sa_handler = sw_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;

	if (withUI) {
		init_ncurses(true);

		sigaction(SIGWINCH, &sa, nullptr);

		resize_terminal();
	}

	sigaction(SIGTERM , &sa, nullptr);
	sigaction(SIGINT  , &sa, nullptr);

	atexit(reset_terminal);

	tcgetattr(STDIN_FILENO, &org_tty_opts);

	struct termios tty_opts_raw { 0 };
	cfmakeraw(&tty_opts_raw);
	tcsetattr(STDIN_FILENO, TCSANOW, &tty_opts_raw);

	struct pollfd fds[] = { { STDIN_FILENO, POLLIN, 0 } };

	uint32_t icount           = 0;
	uint64_t total_icount     = 0;
	uint32_t refresh_interval = 262144;
	constexpr uint32_t pdp_11_70_mips  = 1000000000 / 300;  // 300ns cache

	const unsigned long start          = get_ms();
	unsigned long       interval_start = start;

	for(;;) {
		c->step();

		if (event) {
#if !defined(ESP32)
			FILE *fh = fopen("halt.mac", "wb");
			if (fh) {
				uint16_t pc = 024320;
				fprintf(fh, "\t.LINK %06o\n", pc);

				for(int i=0; i<4096; i += 2)
					fprintf(fh, "\t.DW %06o\n", b->readWord((pc + i) & 0xffff));

				fprintf(fh, "\tmake_raw\n");

				fclose(fh);
			}
#endif

			//c->setRegister(7, 01000);
			//c->resetHalt();
			break;
		}

		icount++;

		if (icount >= refresh_interval) {
			total_icount += icount;

			unsigned long now = get_ms();

			unsigned long took_ms = std::max(1ul, now - interval_start);

			refresh_interval = (1000 * icount) / took_ms;

			if (refresh_interval == 0)
				refresh_interval = 65536;
			else if (refresh_interval > pdp_11_70_mips)
				refresh_interval = pdp_11_70_mips;

			fprintf(stderr, "instructions_executed: %u, took_ms: %lu, new refresh_interval: %u\n", icount, took_ms, refresh_interval);

			if (withUI) {
				mvwprintw(w_main_b -> win, 0, 24, "%.1f/s   ", icount * 1000.0 / took_ms);
				mvwprintw(w_main_b -> win, 0, 42, "%06o", b->get_switch_register());
				mydoupdate();
			}

			if (terminate)
				event = 1;

			interval_start = now;
			icount = 0;
		}
	}

	if (withUI)
		endwin();

	fprintf(stderr, "Instructions per second: %.3f\n\n", icount * 1000.0 / (get_ms() - start));

	delete b;

	return 0;
}
