// (C) 2018-2024 by Folkert van Heusden
// Released under MIT license

#include "disk_backend.h"
#include "disk_backend_file.h"
#include "disk_backend_nbd.h"


disk_backend::disk_backend()
{
}

disk_backend::~disk_backend()
{
}

#if IS_POSIX
disk_backend *disk_backend::deserialize(const json_t *const j)
{
	std::string type = json_string_value(json_object_get(j, "disk-backend-type"));

	if (type == "file")
		return disk_backend_file::deserialize(j);

	if (type == "nbd")
		return disk_backend_nbd::deserialize(j);

	// should not be reached

	return nullptr;
}
#endif
