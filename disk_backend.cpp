// (C) 2018-2024 by Folkert van Heusden
// Released under MIT license

#include <cassert>

#include "disk_backend.h"
#include "gen.h"
#if IS_POSIX
#include "disk_backend_file.h"
#include "disk_backend_nbd.h"
#endif


disk_backend::disk_backend()
{
}

disk_backend::~disk_backend()
{
}

void disk_backend::store_object_in_overlay(const off_t id, const std::vector<uint8_t> & data)
{
	overlay.insert_or_assign(id, data);
}

std::optional<std::vector<uint8_t> > disk_backend::get_object_from_overlay(const off_t id)
{
	auto it = overlay.find(id);
	if (it != overlay.end())
		return it->second;

	return { };
}

std::optional<std::vector<uint8_t> > disk_backend::get_from_overlay(const off_t offset, const size_t sector_size)
{
	assert((offset % sector_size) == 0);

	if (use_overlay)
		return get_object_from_overlay(offset / sector_size);

	return { };
}

bool disk_backend::store_mem_range_in_overlay(const off_t offset, const size_t n, const uint8_t *const from, const size_t sector_size)
{
	assert((offset % sector_size) == 0);
	assert((n % sector_size) == 0);

	if (use_overlay) {
		for(size_t o=0; o<n; o += sector_size)
			store_object_in_overlay((offset + o) / sector_size, std::vector<uint8_t>(from + o, from + o + sector_size));

		return true;
	}

	return false;
}

JsonVariant disk_backend::serialize_overlay() const
{
	JsonVariant out;

	for(auto & id: overlay) {
		JsonVariant j_data;

		for(size_t i=0; i<id.second.size(); i++)
			j_data.add(id.second.at(i));

		out[format("%lu", id.first)] = j_data;
	}

	return out;
}

void disk_backend::deserialize_overlay(const JsonVariant j)
{
	if (j.containsKey("overlay") == false)
		return; // we can have state-dumps without overlay

	for(auto kv : j.as<JsonObject>()) {
		uint32_t id = std::atoi(kv.key().c_str());

		std::vector<uint8_t> data;
		for(auto v: kv.value().as<JsonArray>())
			data.push_back(v);

		store_object_in_overlay(id, data);
	}
}

disk_backend *disk_backend::deserialize(const JsonVariant j)
{
	std::string   type = j["disk-backend-type"];

	disk_backend *d    = nullptr;

	if (type == "file")
		d = disk_backend_file::deserialize(j);

	else if (type == "nbd")
		d = disk_backend_nbd::deserialize(j);

	// should not be triggered
	assert(d);

	d->deserialize_overlay(j);

	// assume we want snapshots (again?)
	d->begin(true);

	return d;
}
