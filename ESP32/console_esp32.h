#include <Arduino.h>

#include "console.h"


class console_esp32 : public console
{
protected:
	int wait_for_char(const int timeout) override;

	void put_char_ll(const char c) override;

public:
	console_esp32(std::atomic_bool *const terminate);
	virtual ~console_esp32();

	void resize_terminal() override;
};
