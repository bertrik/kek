// (C) 2018-2024 by Folkert van Heusden
// Released under MIT license

#if defined(ESP32)
#include <Arduino.h>
#endif
#include <string.h>

#include "memory.h"

memory::memory(const uint32_t size): size(size)
{
#if defined(ESP32)
	Serial.print(F("Memory size (in bytes, decimal): "));
	Serial.println(size);

	if (size > 12 * 8192) {
		Serial.println(F("Using PSRAM"));
		is_psram = true;

		m = reinterpret_cast<uint8_t *>(ps_malloc(size));

		reset();
	}
	else {
		m = new uint8_t[size]();
	}
#else
	m = new uint8_t[size]();
#endif
}

memory::~memory()
{
#if defined(ESP32)
	if (is_psram)
		free(m);
	else
		delete [] m;
#else
	delete [] m;
#endif
}

void memory::reset()
{
	memset(m, 0x00, size);
}

#if IS_POSIX
json_t *memory::serialize() const
{
	json_t *j = json_object();

	json_object_set(j, "size", json_integer(size));

	json_t *ja = json_array();
	for(size_t i=0; i<size; i++)
		json_array_append(ja, json_integer(m[i]));
	json_object_set(j, "contents", ja);

	return j;
}

memory *memory::deserialize(const json_t *const j)
{
	size_t  size = json_integer_value(json_object_get(j, "size"));
	memory *m    = new memory(size);

	json_t *ja   = json_object_get(j, "contents");
	for(size_t i=0; i<size; i++)
		m->m[i] = json_integer_value(json_array_get(ja, i));

	return m;
}
#endif
