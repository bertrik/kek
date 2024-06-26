// (C) 2018-2024 by Folkert van Heusden
// Released under MIT license

#include <errno.h>
#include <string.h>

#include "bus.h"
#include "cpu.h"
#include "error.h"
#include "gen.h"
#include "log.h"
#include "rk05.h"
#include "utils.h"


static const char * const regnames[] = { 
	"RK05_DS drivestatus",
	"RK05_ERROR        ",
	"RK05_CS ctrlstatus",
	"RK05_WC word count",
	"RK05_BA busaddress",
	"RK05_DA disk addrs",
	"RK05_DATABUF      "
	};

rk05::rk05(bus *const b, std::atomic_bool *const disk_read_acitivity, std::atomic_bool *const disk_write_acitivity) :
	b(b),
	disk_read_acitivity(disk_read_acitivity),
	disk_write_acitivity(disk_write_acitivity)
{
}

rk05::~rk05()
{
	for(auto fh : fhs)
		delete fh;
}

void rk05::begin()
{
	reset();
}

void rk05::reset()
{
	memset(registers, 0x00, sizeof registers);
}

uint8_t rk05::read_byte(const uint16_t addr)
{
	uint16_t v = read_word(addr & ~1);

	if (addr & 1)
		return v >> 8;

	return v;
}

uint16_t rk05::read_word(const uint16_t addr)
{
	const int reg = (addr - RK05_BASE) / 2;

	if (addr == RK05_DS) {		// 0177400
		setBit(registers[reg], 11, true); // disk on-line
		setBit(registers[reg], 8, true); // sector ok
		setBit(registers[reg], 7, true); // drive ready
		setBit(registers[reg], 6, true); // seek ready
		setBit(registers[reg], 4, true); // heads in position
	}
	else if (addr == RK05_ERROR)	// 0177402
		registers[reg] = 0;
	else if (addr == RK05_CS) {	// 0177404
		setBit(registers[reg], 15, false); // clear error
		setBit(registers[reg], 14, false); // clear hard error
		setBit(registers[reg], 7, true); // controller ready
	}

	uint16_t vtemp = registers[reg];

	if (addr == RK05_CS)
		setBit(registers[reg], 0, false); // clear go

	DOLOG(debug, false, "RK05 read %s/%o: %06o", reg[regnames], addr, vtemp);

	return vtemp;
}

uint32_t rk05::get_bus_address() const
{
	return registers[(RK05_BA - RK05_BASE) / 2] | (uint32_t((registers[(RK05_CS - RK05_BASE) / 2] >> 4) & 3) << 16);
}

void rk05::update_bus_address(const uint16_t v)
{
	uint32_t org_v = get_bus_address();

	org_v += v;

	registers[(RK05_BA - RK05_BASE) / 2] = org_v;

	registers[(RK05_CS - RK05_BASE) / 2] &= ~(3 << 4);
	registers[(RK05_CS - RK05_BASE) / 2] |= ((org_v >> 16) & 3) << 4;
}

void rk05::write_byte(const uint16_t addr, const uint8_t v)
{
	uint16_t vtemp = registers[(addr - RK05_BASE) / 2];

	update_word(&vtemp, addr & 1, v);

	write_word(addr, vtemp);
}

void rk05::write_word(const uint16_t addr, const uint16_t v)
{
	const int reg = (addr - RK05_BASE) / 2;

	registers[reg] = v;

	if (addr == RK05_CS) {
		if (v & 1) { // GO
			const int    func   = (v >> 1) & 7; // FUNCTION
			int16_t      wc     = registers[(RK05_WC - RK05_BASE) / 2];
			const size_t reclen = wc < 0 ? (-wc * 2) : wc * 2;

			uint16_t temp     = registers[(RK05_DA - RK05_BASE) / 2];
			uint8_t  sector   = temp & 15;
			uint8_t  surface  = (temp >> 4) & 1;
			int      track    = (temp >> 4) & 511;
			uint16_t cylinder = (temp >> 5) & 255;
			uint16_t device   = temp >> 13;

			const uint32_t diskoff  = track * 12 + sector;

			const uint32_t diskoffb = diskoff * 512l; // RK05 is high density
			const uint32_t memoff   = get_bus_address();

			registers[(RK05_CS - RK05_BASE) / 2] &= ~(1 << 13); // reset search complete

			if (func == 0) { // controller reset
				DOLOG(debug, false, "RK05 invoke %d (controller reset)", func);
			}
			else if (func == 1) { // write
				*disk_write_acitivity = true;

				DOLOG(debug, false, "RK05 drive %d position sec %d surf %d cyl %d, reclen %zo, WRITE to %o, mem: %o", device, sector, surface, cylinder, reclen, diskoffb, memoff);

				uint32_t  work_reclen   = reclen;
				uint32_t  work_memoff   = memoff;
				uint32_t  work_diskoffb = diskoffb;

				assert(sizeof(xfer_buffer) == 512);

				while(work_reclen > 0) {
					uint32_t cur = std::min(uint32_t(sizeof xfer_buffer), work_reclen);
					work_reclen -= cur;

					for(size_t i=0; i<cur; i++)
						xfer_buffer[i] = b->readUnibusByte(work_memoff++);

					if (!fhs.at(device)->write(work_diskoffb, cur, xfer_buffer, 512))
						DOLOG(ll_error, true, "RK05(%d) write error %s to %u len %u", device, strerror(errno), work_diskoffb, cur);

					work_diskoffb += cur;

					if (v & 2048)
						DOLOG(debug, false, "RK05 inhibit BA increase");
					else
						update_bus_address(cur);

					if (++sector >= 12) {
						sector = 0;
						if (++surface >= 2) {
							surface = 0;
							cylinder++;
						}
					}
				}

				registers[(RK05_DA - RK05_BASE) / 2] = sector | (surface << 4) | (cylinder << 5);

				*disk_write_acitivity = false;
			}
			else if (func == 2) { // read
				*disk_read_acitivity = true;

				DOLOG(debug, false, "RK05 drive %d position sec %d surf %d cyl %d, reclen %zo, READ from %o, mem: %o", device, sector, surface, cylinder, reclen, diskoffb, memoff);

				uint32_t temp_diskoffb = diskoffb;

				uint32_t temp_reclen   = reclen;
				uint32_t p             = memoff;
				while(temp_reclen > 0) {
					uint32_t cur = std::min(uint32_t(sizeof xfer_buffer), temp_reclen);

					if (!fhs.at(device)->read(temp_diskoffb, cur, xfer_buffer, 512)) {
						DOLOG(ll_error, true, "RK05 read error %s from %u len %u", strerror(errno), temp_diskoffb, cur);
						break;
					}

					temp_diskoffb += cur;

					for(uint32_t i=0; i<cur; i++) {
						b->writeUnibusByte(p++, xfer_buffer[i]);

						if ((v & 2048) == 0)
							update_bus_address(2);
					}

					temp_reclen -= cur;

					if (++sector >= 12) {
						sector = 0;

						if (++surface >= 2) {
							surface = 0;
							cylinder++;
						}
					}
				}

				registers[(RK05_DA - RK05_BASE) / 2] = sector | (surface << 4) | (cylinder << 5);

				*disk_read_acitivity = false;
			}
			else if (func == 4) {
				DOLOG(debug, false, "RK05 invoke %d (seek) to %o", func, diskoffb);

				registers[(RK05_CS - RK05_BASE) / 2] |= 1 << 13; // search complete
			}
			else if (func == 7) {
				DOLOG(debug, false, "RK05 invoke %d (write lock)", func);
			}
			else {
				DOLOG(debug, false, "RK05 command %d UNHANDLED", func);
			}

			registers[(RK05_WC - RK05_BASE) / 2] = 0;

			registers[(RK05_DS - RK05_BASE) / 2] |= 64;  // drive ready
			registers[(RK05_CS - RK05_BASE) / 2] |= 128;  // control ready

			// bit 6, invoke interrupt when done vector address 220, see http://www.pdp-11.nl/peripherals/disk/rk05-info.html
			if (v & 64) {
				registers[(RK05_DS - RK05_BASE) / 2] &= ~(7l << 13);  // store id of the device that caused the interrupt
				registers[(RK05_DS - RK05_BASE) / 2] |= device << 13;

				b->getCpu()->queue_interrupt(5, 0220);
			}
		}
	}
}

#if IS_POSIX
json_t *rk05::serialize() const
{
	json_t *j = json_object();

	json_t *j_backends = json_array();
	for(auto & dbe: fhs)
		json_array_append(j_backends, dbe->serialize());

	json_object_set(j, "backends", j_backends);

	for(int regnr=0; regnr<7; regnr++)
		json_object_set(j, format("register-%d", regnr).c_str(), json_integer(registers[regnr]));

	return j;
}

rk05 *rk05::deserialize(const json_t *const j, bus *const b)
{
	std::vector<disk_backend *> backends;

	rk05 *r = new rk05(b, nullptr, nullptr);
	r->begin();

	json_t *j_backends = json_object_get(j, "backends");
	for(size_t i=0; i<json_array_size(j_backends); i++)
		r->access_disk_backends()->push_back(disk_backend::deserialize(json_array_get(j_backends, i)));

	for(int regnr=0; regnr<7; regnr++)
		r->registers[regnr] = json_integer_value(json_object_get(j, format("register-%d", regnr).c_str()));

	return r;
}
#endif
