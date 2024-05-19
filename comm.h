// (C) 2024 by Folkert van Heusden
// Released under MIT license

#pragma once

#include "gen.h"
#include <cstddef>
#include <cstdint>
#include <string>


class comm
{
public:
	comm();
	virtual ~comm();

	virtual bool    begin() = 0;

	virtual std::string get_identifier() const = 0;

	virtual bool    is_connected() = 0;

	virtual bool    has_data() = 0;
	virtual uint8_t get_byte() = 0;

	virtual void    send_data(const uint8_t *const in, const size_t n) = 0;

        void            println(const char *const s);
        void            println(const std::string & in);
};
