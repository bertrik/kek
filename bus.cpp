// (C) 2018-2022 by Folkert van Heusden
// Released under Apache License v2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "gen.h"
#include "cpu.h"
#include "log.h"
#include "memory.h"
#include "tm-11.h"
#include "tty.h"

#if defined(ESP32)
// ESP32 goes in a crash-loop when allocating 128kB
// see also https://github.com/espressif/esp-idf/issues/1934
constexpr int n_pages = 12;
#else
constexpr int n_pages = 30;  // 30=240kB (for EKBEEx.BIC)
#endif

constexpr uint16_t di_ena_mask[4] = { 4, 2, 0, 1 };

bus::bus()
{
	m = new memory(n_pages * 8192);

	memset(pages, 0x00, sizeof pages);

	CPUERR = MMR0 = MMR1 = MMR2 = MMR3 = PIR = CSR = 0;
}

bus::~bus()
{
	delete c;
	delete tm11;
	delete rk05_;
	delete rl02_;
	delete tty_;
	delete m;
}

void bus::add_cpu(cpu *const c)
{
	delete this->c;
	this->c     = c;
}

void bus::add_tm11(tm_11 *const tm11)
{
	delete this->tm11;
	this->tm11  = tm11;
} 

void bus::add_rk05(rk05 *const rk05_)
{
	delete this->rk05_;
	this->rk05_ = rk05_;
} 

void bus::add_rl02(rl02 *const rl02_)
{
	delete this->rl02_;
	this->rl02_ = rl02_;
}

void bus::add_tty(tty *const tty_)
{
	delete this->tty_;
	this->tty_  = tty_;
}

void bus::clearmem()
{
	m->reset();
}

void bus::init()
{
	MMR0 = 0;
	MMR3 = 0;
}

uint16_t bus::read_pdr(const uint32_t a, const int run_mode, const bool word_mode, const bool peek_only)
{
	int      page = (a >> 1) & 7;
	bool     is_d = a & 16;
	uint16_t t    = pages[run_mode][is_d][page].pdr;

	if (!peek_only)
		DOLOG(debug, false, "READ-I/O PDR run-mode %d: %c for %d: %o", run_mode, is_d ? 'D' : 'I', page, t);

	return word_mode ? (a & 1 ? t >> 8 : t & 255) : t;
}

uint16_t bus::read_par(const uint32_t a, const int run_mode, const bool word_mode, const bool peek_only)
{
	int      page = (a >> 1) & 7;
	bool     is_d = a & 16;
	uint16_t t    = pages[run_mode][is_d][page].par;

	if (!peek_only)
		DOLOG(debug, false, "READ-I/O PAR run-mode %d: %c for %d: %o (phys: %07o)", run_mode, is_d ? 'D' : 'I', page, t, t * 64);

	return word_mode ? (a & 1 ? t >> 8 : t & 255) : t;
}

void bus::trap_odd(const uint16_t a)
{
	MMR0 &= ~(7 << 1);
	MMR0 |= (a >> 13) << 1;

	c->trap(004);  // invalid access
}

uint16_t bus::read(const uint16_t a, const bool word_mode, const bool use_prev, const bool peek_only, const d_i_space_t space)
{
	uint16_t temp = 0;

	if (a >= 0160000) {
		//// REGISTERS ////
		if (a >= ADDR_KERNEL_R && a <= ADDR_KERNEL_R + 5) { // kernel R0-R5
			if (!peek_only) DOLOG(debug, false, "READ-I/O kernel R%d", a - ADDR_KERNEL_R);
			return c->getRegister(a - ADDR_KERNEL_R, 0, false) & (word_mode ? 0xff : 0xffff);
		}
		if (a >= ADDR_USER_R && a <= ADDR_USER_R + 5) { // user R0-R5
			if (!peek_only) DOLOG(debug, false, "READ-I/O user R%d", a - ADDR_USER_R);
			return c->getRegister(a - ADDR_USER_R, 3, false) & (word_mode ? 0xff : 0xffff);
		}
		if (a == ADDR_KERNEL_SP) { // kernel SP
			if (!peek_only) DOLOG(debug, false, "READ-I/O kernel SP");
			return c->getStackPointer(0) & (word_mode ? 0xff : 0xffff);
		}
		if (a == ADDR_PC) { // PC
			if (!peek_only) DOLOG(debug, false, "READ-I/O PC");
			return c->getPC() & (word_mode ? 0xff : 0xffff);
		}
		if (a == ADDR_SV_SP) { // supervisor SP
			if (!peek_only) DOLOG(debug, false, "READ-I/O supervisor SP");
			return c->getStackPointer(1) & (word_mode ? 0xff : 0xffff);
		}
		if (a == ADDR_USER_SP) { // user SP
			if (!peek_only) DOLOG(debug, false, "READ-I/O user SP");
			return c->getStackPointer(3) & (word_mode ? 0xff : 0xffff);
		}
		///^ registers ^///

		if (!peek_only) {
			if ((a & 1) && word_mode == false) {
				DOLOG(debug, true, "READ-I/O odd address %06o UNHANDLED", a);
				trap_odd(a);
				throw 0;
				return 0;
			}
		}

		if (a == ADDR_CPU_ERR) { // cpu error register
			if (!peek_only) DOLOG(debug, false, "READ-I/O CPU error");
			return CPUERR & 0xff;
		}

		if (a == ADDR_MAINT) { // MAINT
			if (!peek_only) DOLOG(debug, false, "READ-I/O MAINT");
			return 1; // POWER OK
		}

		if (a == ADDR_CONSW) { // console switch & display register
			if (!peek_only) DOLOG(debug, false, "READ-I/O console switch (%06o)", console_switches);
			return console_switches;
		}

		if (a == ADDR_KW11P) { // KW11P programmable clock
			if (!peek_only) DOLOG(debug, false, "READ-I/O programmable clock");
			return 128;
		}

		if (a == ADDR_PIR || a == ADDR_PIR + 1) { // PIR
			if (!peek_only) DOLOG(debug, false, "READ-I/O PIR");

			if (word_mode == false)
				return PIR;

			return a == ADDR_PIR ? PIR & 255 : PIR >> 8;
		}

		if (a == ADDR_SYSTEM_ID) {
			if (!peek_only) DOLOG(debug, false, "READ-I/O system id");
			return 011064;
		}

		if (a == ADDR_LFC) { // line frequency clock and status register
			if (!peek_only) DOLOG(debug, false, "READ-I/O line freq clock");
			return lf_csr;
		}

		if (a == ADDR_LP11CSR) { // printer, CSR register, LP11
			if (!peek_only) DOLOG(debug, false, "READ-I/O LP11 CSR");
			return 0x80;
		}

		/// MMU ///
		if (a >= ADDR_PDR_SV_START && a < ADDR_PDR_SV_END)
			return read_pdr(a, 1, word_mode, peek_only);
		else if (a >= ADDR_PAR_SV_START && a < ADDR_PAR_SV_END)
			return read_par(a, 1, word_mode, peek_only);
		else if (a >= ADDR_PDR_K_START && a < ADDR_PDR_K_END)
			return read_pdr(a, 0, word_mode, peek_only);
		else if (a >= ADDR_PAR_K_START && a < ADDR_PAR_K_END)
			return read_par(a, 0, word_mode, peek_only);
		else if (a >= ADDR_PDR_U_START && a < ADDR_PDR_U_END)
			return read_pdr(a, 3, word_mode, peek_only);
		else if (a >= ADDR_PAR_U_START && a < ADDR_PAR_U_END)
			return read_par(a, 3, word_mode, peek_only);
		///////////

		if (a >= 0177740 && a <= 0177753) { // cache control register and others
			// TODO
			return 0;
		}

		if (a >= 0170200 && a <= 0170377) { // unibus map
			if (!peek_only) DOLOG(debug, false, "READ-I/O unibus map (%06o)", a);
			// TODO
			return 0;
		}

		if (word_mode) {
			if (a == ADDR_PSW) { // PSW
				if (!peek_only) DOLOG(debug, false, "READ-I/O PSW LSB");
				return c->getPSW() & 255;
			}

			if (a == ADDR_PSW + 1) {
				if (!peek_only) DOLOG(debug, false, "READ-I/O PSW MSB");
				return c->getPSW() >> 8;
			}
			if (a == ADDR_STACKLIM) { // stack limit register
				if (!peek_only) DOLOG(debug, false, "READ-I/O stack limit register (low)");
				return c->getStackLimitRegister() & 0xff;
			}
			if (a == ADDR_STACKLIM + 1) { // stack limit register
				if (!peek_only) DOLOG(debug, false, "READ-I/O stack limit register (high)");
				return c->getStackLimitRegister() >> 8;
			}

			if (a == ADDR_MICROPROG_BREAK_REG) {  // microprogram break register
				if (!peek_only) DOLOG(debug, false, "READ-I/O microprogram break register (low: %03o)", microprogram_break_register & 255);
				return microprogram_break_register & 255;
			}
			if (a == ADDR_MICROPROG_BREAK_REG + 1) {  // microprogram break register
				if (!peek_only) DOLOG(debug, false, "READ-I/O microprogram break register (high: %03o)", microprogram_break_register >> 8);
				return microprogram_break_register >> 8;
			}

			if (a == ADDR_MMR0) {
				if (!peek_only) DOLOG(debug, false, "READ-I/O MMR0 LO");
				return MMR0 & 255;
			}
			if (a == ADDR_MMR0 + 1) {
				if (!peek_only) DOLOG(debug, false, "READ-I/O MMR0 HI");
				return MMR0 >> 8;
			}
		}
		else {
			if (a == ADDR_MMR0) {
				if (!peek_only) DOLOG(debug, false, "READ-I/O MMR0");
				return MMR0;
			}

			if (a == ADDR_MMR1) { // MMR1
				if (!peek_only) DOLOG(debug, false, "READ-I/O MMR1");
				return MMR1;
			}

			if (a == ADDR_MMR2) { // MMR2
				if (!peek_only) DOLOG(debug, false, "READ-I/O MMR2");
				return MMR2;
			}

			if (a == ADDR_MMR3) { // MMR3
				if (!peek_only) DOLOG(debug, false, "READ-I/O MMR3");
				return MMR3;
			}

			if (a == ADDR_PSW) { // PSW
				if (!peek_only) DOLOG(debug, false, "READ-I/O PSW");
				return c->getPSW();
			}

			if (a == ADDR_STACKLIM) { // stack limit register
				if (!peek_only) DOLOG(debug, false, "READ-I/O stack limit register");
				return c->getStackLimitRegister();
			}

			if (a == ADDR_CPU_ERR) { // cpu error register
				if (!peek_only) DOLOG(debug, false, "READ-I/O CPUERR");
				return CPUERR;
			}

			if (a == ADDR_MICROPROG_BREAK_REG) {  // microprogram break register
				if (!peek_only) DOLOG(debug, false, "READ-I/O micropgrogram break register (%06o)", microprogram_break_register);
				return microprogram_break_register;
			}
		}

		if (tm11 && a >= TM_11_BASE && a < TM_11_END && !peek_only) {
			DOLOG(debug, false, "READ-I/O TM11 register %d", (a - TM_11_BASE) / 2);

			return word_mode ? tm11->readByte(a) : tm11->readWord(a);
		}

		if (rk05_ && a >= RK05_BASE && a < RK05_END && !peek_only) {
			DOLOG(debug, false, "READ-I/O RK05 register %d", (a - RK05_BASE) / 2);

			return word_mode ? rk05_->readByte(a) : rk05_->readWord(a);
		}

		if (rl02_ && a >= RL02_BASE && a < RL02_END && !peek_only) {
			DOLOG(debug, false, "READ-I/O RL02 register %d", (a - RL02_BASE) / 2);

			return word_mode ? rl02_->readByte(a) : rl02_->readWord(a);
		}

		if (tty_ && a >= PDP11TTY_BASE && a < PDP11TTY_END && !peek_only) {
			DOLOG(debug, false, "READ-I/O RL02 register %d", (a - RL02_BASE) / 2);

			return word_mode ? tty_->readByte(a) : tty_->readWord(a);
		}

		// LO size register field must be all 1s, so subtract 1
		constexpr uint32_t system_size = n_pages * 8192 / 64 - 1;

		if (a == ADDR_SYSSIZE + 2) {  // system size HI
			if (!peek_only) DOLOG(debug, false, "READ-I/O accessing system size HI");
			return system_size >> 16;
		}

		if (a == ADDR_SYSSIZE) {  // system size LO
			if (!peek_only) DOLOG(debug, false, "READ-I/O accessing system size LO");
			return system_size;
		}

		if (!peek_only) {
			DOLOG(debug, true, "READ-I/O UNHANDLED read %06o (%c)", a, word_mode ? 'B' : ' ');

			c->trap(004);  // no such i/o

			throw 1;
		}

		return -1;
	}

	if (peek_only == false && word_mode == false && (a & 1)) {
		if (!peek_only) DOLOG(debug, true, "READ from %06o - odd address!", a);
		trap_odd(a);
		throw 2;
		return 0;
	}

	int      run_mode = (c->getPSW() >> (use_prev ? 12 : 14)) & 3;

	uint32_t m_offset = calculate_physical_address(run_mode, a, !peek_only, false, peek_only, space == d_space);

	if (peek_only == false && m_offset >= n_pages * 8192) {
		if (!peek_only) DOLOG(debug, true, "Read non existing mapped memory (%o >= %o)", m_offset, n_pages * 8192);

		c->trap(004);  // no such memory

		throw 3;
	}

	if (word_mode)
		temp = m->readByte(m_offset);
	else
		temp = m->readWord(m_offset);

	if (!peek_only) DOLOG(debug, false, "READ from %06o/%07o %c %c: %o", a, m_offset, space == d_space ? 'D' : 'I', word_mode ? 'B' : 'W', temp);

	return temp;
}

void bus::setMMR0(int value)
{
	value &= ~(3 << 10);  // bit 10 & 11 always read as 0

	if (value & 1)
		value &= ~(7 << 13);  // reset error bits

	if (MMR0 & 0160000) {
		if ((value & 1) == 0)
			value &= 254;  // bits 7...1 are protected 
	}

// TODO if bit 15/14/13 are set (either of them), then do not modify bit 1...7

	MMR0 = value;
}

void bus::setMMR0Bit(const int bit)
{
	assert(bit != 10 && bit != 11);
	assert(bit < 16 && bit >= 0);

	MMR0 |= 1 << bit;
}

void bus::clearMMR0Bit(const int bit)
{
	assert(bit != 10 && bit != 11);
	assert(bit < 16 && bit >= 0);

	MMR0 &= ~(1 << bit);
}

void bus::setMMR2(const uint16_t value) 
{
	MMR2 = value;
}

void bus::check_odd_addressing(const uint16_t a, const int run_mode, const d_i_space_t space, const bool is_write)
{
	if (a & 1) {
		if (is_write)
			pages[run_mode][space == d_space][a >> 13].pdr |= 1 << 7;

		trap_odd(a);

		throw 4;
	}
}

memory_addresses_t bus::calculate_physical_address(const int run_mode, const uint16_t a)
{
	const uint8_t apf = a >> 13; // active page field

	if ((MMR0 & 1) == 0)
		return { a, apf, a, a };

	uint32_t physical_instruction = pages[run_mode][0][apf].par * 64;
	uint32_t physical_data        = pages[run_mode][1][apf].par * 64;

	uint16_t p_offset = a & 8191;  // page offset

	physical_instruction += p_offset;
	physical_data        += p_offset;

	if ((MMR3 & 16) == 0) {  // offset is 18bit
		physical_instruction &= 0x3ffff;
		physical_data        &= 0x3ffff;
	}

	return { a, apf, physical_instruction, physical_data };
}

bool bus::get_use_data_space(const int run_mode)
{
	return !!(MMR3 & di_ena_mask[run_mode]);
}

uint32_t bus::calculate_physical_address(const int run_mode, const uint16_t a, const bool trap_on_failure, const bool is_write, const bool peek_only, const bool is_data)
{
	uint32_t m_offset = a;

	if ((MMR0 & 1) || (is_write && (MMR0 & (1 << 8)))) {
		const uint8_t apf = a >> 13; // active page field

		bool          d   = is_data & (!!(MMR3 & di_ena_mask[run_mode])) ? is_data : false;

		uint16_t p_offset = a & 8191;  // page offset

		m_offset  = pages[run_mode][d][apf].par * 64;  // memory offset  TODO: handle 16b int-s

		m_offset += p_offset;

		if ((MMR3 & 16) == 0)  // off is 18bit
			m_offset &= 0x3ffff;

		if (trap_on_failure) {
			{
				const int access_control = pages[run_mode][d][apf].pdr & 7;

				bool do_trap = false;

				if (is_write && access_control != 6)  // write
					do_trap = true;
				else if (!is_write && (access_control == 0 || access_control == 1 || access_control == 3 || access_control == 4 || access_control == 7)) {  // read
					do_trap = true;
				}

				if (do_trap) {
					bool do_trap_250 = false;

					if ((MMR0 & 0xf000) == 0) {
						DOLOG(debug, true, "TRAP(0250) (throw 5) for access_control %d on address %06o, run mode %d", access_control, a, run_mode);

						do_trap_250 = true;
					}
					else {
						DOLOG(debug, true, "A.C.F. triggger for %d on address %06o, run mode %d", access_control, a, run_mode);
					}

					if (is_write)
						pages[run_mode][d][apf].pdr |= 1 << 7;

					if ((MMR0 & 0160000) == 0) {
						MMR0 &= ~((1 << 15) | (1 << 14) | (1 << 13) | (1 << 12) | (3 << 5) | (7 << 1));

						if (is_write && access_control != 6)
							MMR0 |= 1 << 13;  // read-only
								  //
						if (access_control == 0 || access_control == 4)
							MMR0 |= 1 << 15;  // not resident
						else
							MMR0 |= 1 << 13;  // read-only

						MMR0 |= run_mode << 5;  // TODO: kernel-mode or user-mode when a trap occurs in user-mode?

						MMR0 |= apf << 1; // add current page
					}

					DOLOG(debug, true, "MMR0: %06o", MMR0);

					if (do_trap_250) {
						c->trap(0250);  // invalid address

						throw 5;
					}
				}
			}

			if (m_offset >= n_pages * 8192) {
				DOLOG(debug, !peek_only, "bus::calculate_physical_address %o >= %o", m_offset, n_pages * 8192);
				DOLOG(debug, true, "TRAP(04) (throw 6) on address %06o", a);

				if ((MMR0 & 0160000) == 0) {
					MMR0 &= 017777;
					MMR0 |= 1 << 15;  // non-resident

					MMR0 &= ~14;  // add current page
					MMR0 |= apf << 1;

					MMR0 &= ~(3 << 5);
					MMR0 |= run_mode << 5;
				}

				if (is_write)
					pages[run_mode][d][apf].pdr |= 1 << 7;

				c->trap(04);

				throw 6;
			}

			uint16_t pdr_len = (pages[run_mode][d][apf].pdr >> 8) & 127;
			uint16_t pdr_cmp = (a >> 6) & 127;

			bool direction = pages[run_mode][d][apf].pdr & 8;

			// DOLOG(debug, true, "p_offset %06o pdr_len %06o direction %d, run_mode %d, apf %d, pdr: %06o", p_offset, pdr_len, direction, run_mode, apf, pages[run_mode][d][apf].pdr);

			if ((pdr_cmp > pdr_len && direction == false) || (pdr_cmp < pdr_len && direction == true)) {
				DOLOG(debug, !peek_only, "bus::calculate_physical_address::p_offset %o versus %o direction %d", pdr_cmp, pdr_len, direction);
				DOLOG(debug, true, "TRAP(0250) (throw 7) on address %06o", a);
				c->trap(0250);  // invalid access

				if ((MMR0 & 0160000) == 0) {
					MMR0 &= 017777;
					MMR0 |= 1 << 14;  // length

					MMR0 &= ~14;  // add current page
					MMR0 |= apf << 1;

					MMR0 &= ~(3 << 5);
					MMR0 |= run_mode << 5;
				}

				if (is_write)
					pages[run_mode][d][apf].pdr |= 1 << 7;

				throw 7;
			}
		}

		DOLOG(debug, !peek_only, "virtual address %06o maps to physical address %08o (run_mode: %d, apf: %d, par: %08o, poff: %o, AC: %d, %s)", a, m_offset, run_mode, apf, pages[run_mode][d][apf].par * 64, p_offset, pages[run_mode][d][apf].pdr & 7, d ? "D" : "I");
	}

	return m_offset;
}

void bus::clearMMR1()
{
	MMR1 = 0;
}

void bus::addToMMR1(const int8_t delta, const uint8_t reg)
{
	MMR1 <<= 8;

	MMR1 |= (delta & 31) << 3;
	MMR1 |= reg;
}

void bus::write_pdr(const uint32_t a, const int run_mode, const uint16_t value, const bool word_mode)
{
	bool is_d = a & 16;
	int  page = (a >> 1) & 7;

	if (word_mode) {
		assert(a != 0 || value < 256);

		a & 1 ? (pages[run_mode][is_d][page].pdr &= 0x00ff, pages[run_mode][is_d][page].pdr |= value << 8) :
#ifdef SYSTEM_11_44
			(pages[run_mode][is_d][page].pdr &= 0xff00, pages[run_mode][is_d][page].pdr |= value & ~0361);
#else
			(pages[run_mode][is_d][page].pdr &= 0xff00, pages[run_mode][is_d][page].pdr |= value     );
#endif
	}
	else {
#ifdef SYSTEM_11_44
		pages[run_mode][is_d][page].pdr = value & ~0361;
#else
		pages[run_mode][is_d][page].pdr = value;
#endif
	}

	pages[run_mode][is_d][page].pdr &= ~(32768 + 128 /*A*/ + 64 /*W*/ + 32 + 16);  // set bit 4, 5 & 15 to 0 as they are unused and A/W are set to 0 by writes

	DOLOG(debug, true, "WRITE-I/O PDR run-mode %d: %c for %d: %o [%d]", run_mode, is_d ? 'D' : 'I', page, value, word_mode);
}

void bus::write_par(const uint32_t a, const int run_mode, const uint16_t value, const bool word_mode)
{
	bool is_d = a & 16;
	int  page = (a >> 1) & 7;

	if (word_mode) {
		a & 1 ? (pages[run_mode][is_d][page].par &= 0x00ff, pages[run_mode][is_d][page].par |= value << 8) :
			(pages[run_mode][is_d][page].par &= 0xff00, pages[run_mode][is_d][page].par |= value     );
	}
	else {
		pages[run_mode][is_d][page].par = value;
	}

	pages[run_mode][is_d][page].pdr &= ~(128 /*A*/ + 64 /*W*/);  // reset PDR A/W when PAR is written to

	DOLOG(debug, true, "WRITE-I/O PAR run-mode %d: %c for %d: %o (%07o)", run_mode, is_d ? 'D' : 'I', page, word_mode ? value & 0xff : value, pages[run_mode][is_d][page].par * 64);
}

void bus::write(const uint16_t a, const bool word_mode, uint16_t value, const bool use_prev, const d_i_space_t space)
{
	int run_mode = (c->getPSW() >> (use_prev ? 12 : 14)) & 3;

	if ((MMR0 & 1) == 1 && (a & 1) == 0 && a != ADDR_MMR0) {
		const uint8_t apf     = a >> 13; // active page field

		bool          is_data = space == d_space;

		bool          d       = is_data & (!!(MMR3 & di_ena_mask[run_mode])) ? is_data : false;

                pages[run_mode][d][apf].pdr |= 64;  // set 'W' (written to) bit
	}

	if (a >= 0160000) {
		if (word_mode) {
			if (a == ADDR_PSW || a == ADDR_PSW + 1) { // PSW
				DOLOG(debug, true, "WRITE-I/O PSW %s: %03o", a & 1 ? "MSB" : "LSB", value);

				uint16_t vtemp = c->getPSW();

				if (a == ADDR_PSW)
					vtemp = (vtemp & 0xff00) | value;
				else
					vtemp = (vtemp & 0x00ff) | (value << 8);

				vtemp &= ~16;  // cannot set T bit via this

				c->setPSW(vtemp, false);

				return;
			}

			if (a == ADDR_STACKLIM || a == ADDR_STACKLIM + 1) { // stack limit register
				DOLOG(debug, true, "WRITE-I/O stack limit register %s: %03o", a & 1 ? "MSB" : "LSB", value);

				if (a == ADDR_STACKLIM + 1) {
					uint16_t v = c->getStackLimitRegister();

					v = (v & 0x00ff) | (value << 8);

					c->setStackLimitRegister(v);
				}

				return;
			}

			if (a == ADDR_MICROPROG_BREAK_REG || a == ADDR_MICROPROG_BREAK_REG + 1) {  // microprogram break register
				DOLOG(debug, false, "WRITE-I/O micropram break register %s: %03o", a & 1 ? "MSB" : "LSB", value);

				if (a == ADDR_MICROPROG_BREAK_REG)
					microprogram_break_register = (microprogram_break_register & 0xff00) | value;
				else
					microprogram_break_register = (microprogram_break_register & 0x00ff) | (value << 8);

				return;
			}

			if (a == ADDR_MMR0 || a == ADDR_MMR0 + 1) { // MMR0
				DOLOG(debug, true, "WRITE-I/O MMR0 register %s: %03o", a & 1 ? "MSB" : "LSB", value);

				if (a == ADDR_MMR0)
					MMR0 = (MMR0 & 0xff00) | value;
				else
					MMR0 = (MMR0 & 0x00ff) | (value << 8);

				return;
			}
		}
		else {
			if (a == ADDR_PSW) { // PSW
				DOLOG(debug, true, "WRITE-I/O PSW: %06o", value);
				c->setPSW(value & ~16, false);
				return;
			}

			if (a == ADDR_STACKLIM) { // stack limit register
				DOLOG(debug, true, "WRITE-I/O stack limit register: %06o", value);
				c->setStackLimitRegister(value & 0xff00);
				return;
			}

			if (a >= ADDR_KERNEL_R && a <= ADDR_KERNEL_R + 5) { // kernel R0-R5
				int reg = a - ADDR_KERNEL_R;
				DOLOG(debug, true, "WRITE-I/O kernel R%d: %06o", reg, value);
				c->setRegister(reg, false, false, value);
				return;
			}
			if (a >= ADDR_USER_R && a <= ADDR_USER_R + 5) { // user R0-R5
				int reg = a - ADDR_USER_R;
				DOLOG(debug, true, "WRITE-I/O user R%d: %06o", reg, value);
				c->setRegister(reg, true, false, value);
				return;
			}
			if (a == ADDR_KERNEL_SP) { // kernel SP
				DOLOG(debug, true, "WRITE-I/O kernel SP: %06o", value);
				c->setStackPointer(0, value);
				return;
			}
			if (a == ADDR_PC) { // PC
				DOLOG(debug, true, "WRITE-I/O PC: %06o", value);
				c->setPC(value);
				return;
			}
			if (a == ADDR_SV_SP) { // supervisor SP
				DOLOG(debug, true, "WRITE-I/O supervisor sp: %06o", value);
				c->setStackPointer(1, value);
				return;
			}
			if (a == ADDR_USER_SP) { // user SP
				DOLOG(debug, true, "WRITE-I/O user sp: %06o", value);
				c->setStackPointer(3, value);
				return;
			}

			if (a == ADDR_MICROPROG_BREAK_REG) {  // microprogram break register
				DOLOG(debug, false, "WRITE-I/O micropgrogram break register: %06o", value);
				microprogram_break_register = value & 0xff; // only 8b on 11/70?
				return;
			}
		}

		if (a == ADDR_CPU_ERR) { // cpu error register
			DOLOG(debug, true, "WRITE-I/O CPUERR: %06o", value);
			CPUERR = 0;
			return;
		}

		if (a == ADDR_MMR3) { // MMR3
			DOLOG(debug, true, "WRITE-I/O set MMR3: %06o", value);
			MMR3 = value & 067;
			return;
		}

		if (a == ADDR_MMR0) { // MMR0
			DOLOG(debug, true, "WRITE-I/O set MMR0: %06o", value);
			setMMR0(value);
			return;
		}

		if (a == ADDR_PIR) { // PIR
			DOLOG(debug, true, "WRITE-I/O set PIR: %06o", value);

			value &= 0177000;

			int bits = value >> 9;

			while(bits) {
				value += 042;  // bit 1...3 and 5...7
				bits >>= 1;
			}

			PIR = value;
			return;
		}

		if (a == ADDR_LFC) { // line frequency clock and status register
			DOLOG(debug, true, "WRITE-I/O set LFC/SR: %06o", value);
			lf_csr = value;
			return;
		}

		if (tm11 && a >= TM_11_BASE && a < TM_11_END) {
			DOLOG(debug, false, "WRITE-I/O TM11 register %d: %06o", (a - TM_11_BASE) / 2, value);
			word_mode ? tm11->writeByte(a, value) : tm11->writeWord(a, value);
			return;
		}

		if (rk05_ && a >= RK05_BASE && a < RK05_END) {
			DOLOG(debug, false, "WRITE-I/O RK05 register %d: %06o", (a - RK05_BASE) / 2, value);
			word_mode ? rk05_->writeByte(a, value) : rk05_->writeWord(a, value);
			return;
		}

		if (rl02_ && a >= RL02_BASE && a < RL02_END) {
			DOLOG(debug, false, "WRITE-I/O RL02 register %d: %06o", (a - RL02_BASE) / 2, value);
			word_mode ? rl02_->writeByte(a, value) : rl02_->writeWord(a, value);
			return;
		}

		if (tty_ && a >= PDP11TTY_BASE && a < PDP11TTY_END) {
			DOLOG(debug, false, "WRITE-I/O TTY register %d: %06o", (a - PDP11TTY_BASE) / 2, value);
			word_mode ? tty_->writeByte(a, value) : tty_->writeWord(a, value);
			return;
		}

		/// MMU ///
		// supervisor
		if (a >= ADDR_PDR_SV_START && a < ADDR_PDR_SV_END) {
			write_pdr(a, 1, value, word_mode);
			return;
		}
		if (a >= ADDR_PAR_SV_START && a < ADDR_PAR_SV_END) {
			write_par(a, 1, value, word_mode);
			return;
		}

		// kernel
		if (a >= ADDR_PDR_K_START && a < ADDR_PDR_K_END) {
			write_pdr(a, 0, value, word_mode);
			return;
		}
		if (a >= ADDR_PAR_K_START && a < ADDR_PAR_K_END) {
			write_par(a, 0, value, word_mode);
			return;
		}

		// user
		if (a >= ADDR_PDR_U_START && a < ADDR_PDR_U_END) {
			write_pdr(a, 3, value, word_mode);
			return;
		}
		if (a >= ADDR_PAR_U_START && a < ADDR_PAR_U_END) {
			write_par(a, 3, value, word_mode);
			return;
		}
		////

		if (a >= 0177740 && a <= 0177753) { // cache control register and others
			// TODO
			return;
		}

		if (a >= 0170200 && a <= 0170377) { // unibus map
			DOLOG(debug, false, "writing %06o to unibus map (%06o)", value, a);
			// TODO
			return;
		}

		if (a == ADDR_CONSW) {  // switch register
			console_leds = value;
			return;
		}

		if (a == ADDR_SYSSIZE || a == ADDR_SYSSIZE + 2)  // system size (is read-only)
			return;

		if (a == ADDR_SYSTEM_ID)  // is r/o
			return;

		///////////

		DOLOG(debug, true, "WRITE-I/O UNHANDLED %06o(%c): %06o", a, word_mode ? 'B' : 'W', value);

		if (word_mode == false && (a & 1)) {
			DOLOG(debug, true, "WRITE-I/O to %06o (value: %06o) - odd address!", a, value);

			trap_odd(a);
			throw 8;
		}

		c->trap(004);  // no such i/o

		throw 9;
	}

	if (word_mode == false && (a & 1)) {
		DOLOG(debug, true, "WRITE to %06o (value: %06o) - odd address!", a, value);

		trap_odd(a);
		throw 10;
	}

	uint32_t m_offset = calculate_physical_address(run_mode, a, true, true, false, space == d_space);

	if (m_offset >= n_pages * 8192) {
		DOLOG(debug, false, "Write non existing mapped memory (%06o, value: %06o)", m_offset, value);
		c->trap(004);  // no such memory
		throw 11;
	}

	DOLOG(debug, true, "WRITE to %06o/%07o %c %c: %06o", a, m_offset, space == d_space ? 'D' : 'I', word_mode ? 'B' : 'W', value);

	if (word_mode)
		m->writeByte(m_offset, value);
	else
		m->writeWord(m_offset, value);
}

void bus::writePhysical(const uint32_t a, const uint16_t value)
{
	DOLOG(debug, true, "physicalWRITE %06o to %o", value, a);

	if (a >= n_pages * 8192) {
		DOLOG(debug, true, "physicalWRITE to %o: trap 004", a);
		c->trap(004);
		throw 12;
	}
	else {
		m->writeWord(a, value);
	}
}

uint16_t bus::readPhysical(const uint32_t a)
{
	if (a >= n_pages * 8192) {
		DOLOG(debug, true, "physicalREAD from %o: trap 004", a);
		c->trap(004);
		throw 13;
	}

	uint16_t value = m->readWord(a);

	DOLOG(debug, true, "physicalREAD %06o from %o", value, a);

	return value;
}

uint16_t bus::readWord(const uint16_t a, const d_i_space_t s)
{
	return read(a, false, false, false, s);
}

uint16_t bus::peekWord(const uint16_t a)
{
	return read(a, false, false, true);
}

void bus::writeWord(const uint16_t a, const uint16_t value, const d_i_space_t s)
{
	write(a, false, value, false, s);
}

uint16_t bus::readUnibusByte(const uint16_t a)
{
	return m->readByte(a);
}

void bus::writeUnibusByte(const uint16_t a, const uint8_t v)
{
	m->writeByte(a, v);
}

void bus::set_lf_crs_b7()
{
	lf_csr |= 128;
}

uint8_t bus::get_lf_crs()
{
	return lf_csr;
}
