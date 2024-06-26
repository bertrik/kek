// (C) 2018-2024 by Folkert van Heusden
// Released under MIT license

#include <string>

#include "disk_backend.h"


class disk_backend_file : public disk_backend
{
private:
	const std::string filename;

	int fd { -1 };

public:
	disk_backend_file(const std::string & filename);
	virtual ~disk_backend_file();

#if IS_POSIX
	json_t *serialize() const override;
	static disk_backend_file *deserialize(const json_t *const j);
#endif

	std::string get_identifier() const override { return filename; }

	bool begin(const bool snapshots) override;

	bool read(const off_t offset, const size_t n, uint8_t *const target, const size_t sector_size) override;

	bool write(const off_t offset, const size_t n, const uint8_t *const from, const size_t sector_size) override;
};
