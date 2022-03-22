// (C) 2018-2022 by Folkert van Heusden
// Released under Apache License v2.0
#include <Adafruit_NeoPixel.h>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <WiFi.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "console_esp32.h"
#include "cpu.h"
#include "error.h"
#include "esp32.h"
#include "memory.h"
#include "tty.h"
#include "utils.h"


#define NEOPIXELS_PIN	25

bus     *b    = nullptr;
cpu     *c    = nullptr;
tty     *tty_ = nullptr;
console *cnsl = nullptr;

uint32_t event     = 0;

uint16_t exec_addr = 0;

uint32_t start_ts  = 0;

std::atomic_bool terminate { false };

std::atomic_bool running   { false };
std::atomic_bool on_wifi   { false };
std::atomic_bool disk_read_activity  { false };
std::atomic_bool disk_write_activity { false };

void setBootLoader(bus *const b) {
	cpu     *const c      = b->getCpu();

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

	for(size_t i=0; i<sizeof bootrom / 2; i++)
		b->writeWord(offset + i * 2, bootrom[i]);

	c->setRegister(7, offset);
}

void panel(void *p) {
	Serial.println(F("panel task started"));

	bus *const b = reinterpret_cast<bus *>(p);
	cpu *const c = b->getCpu();

	constexpr const uint8_t n_leds = 60;
	Adafruit_NeoPixel pixels(n_leds, NEOPIXELS_PIN, NEO_RGBW);
	pixels.begin();

	pixels.clear();

	pixels.setBrightness(48);
	pixels.show();

	const uint32_t magenta = pixels.Color(255, 0, 255);
	const uint32_t red     = pixels.Color(255, 0, 0);
	const uint32_t green   = pixels.Color(0, 255, 0);
	const uint32_t blue    = pixels.Color(0, 0, 255);
	const uint32_t yellow  = pixels.Color(255, 255, 0);
	const uint32_t white   = pixels.Color(255, 255, 255, 255);

	const uint32_t run_mode_led_color[4] = { red, yellow, blue, green };

	// initial animation
	for(uint8_t i=0; i<n_leds; i++) {
		pixels.setPixelColor(i, 255, 255, 255);

		int p = i - 10;
		if (p < 0)
			p += n_leds;

		pixels.setPixelColor(p, 0, 0, 0);

		pixels.show();

		delay(10);
	}

	pixels.clear();
	pixels.show();

	for(;;) {
		vTaskDelay(20 / portTICK_RATE_MS);

		// note that these are approximately as there's no mutex on the emulation
		uint16_t current_PC    = c->getPC();
		uint32_t full_addr     = b->calculate_full_address(current_PC);

		uint16_t current_instr = b->readWord(current_PC);

		uint16_t current_PSW   = c->getPSW();

		uint32_t led_color     = run_mode_led_color[current_PSW >> 14];

		for(uint8_t b=0; b<22; b++)
			pixels.setPixelColor(b, full_addr & (1 << b) ? led_color : 0);

		for(uint8_t b=0; b<16; b++)
			pixels.setPixelColor(b + 22, current_PSW & (1 << b) ? magenta : 0);

		for(uint8_t b=0; b<16; b++)
			pixels.setPixelColor(b + 38, current_instr & (1 << b) ? red : 0);

		pixels.setPixelColor(54, running ? white : 0);

		pixels.setPixelColor(55, on_wifi ? white : 0);

		pixels.setPixelColor(56, disk_read_activity  ? blue : 0);
		pixels.setPixelColor(57, disk_write_activity ? blue : 0);

		pixels.show();
	}
}

void setup_wifi_stations()
{
#if 0
	WiFi.mode(WIFI_STA);

	WiFi.softAP("PDP-11 KEK", nullptr, 5, 0, 4);

#if 0
	Serial.println(F("Scanning for WiFi access points..."));

	int n = WiFi.scanNetworks();

	Serial.println(F("scan done"));

	if (n == 0)
		Serial.println(F("no networks found"));
	else {
		for (int i = 0; i < n; ++i) {
			// Print SSID and RSSI for each network found
			Serial.print(i + 1);
			Serial.print(F(": "));
			Serial.print(WiFi.SSID(i));
			Serial.print(F(" ("));
			Serial.print(WiFi.RSSI(i));
			Serial.print(F(")"));
			Serial.println(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " " : "*");
			delay(10);
		}
	}

	std::string ssid = read_terminal_line("SSID: ");
	std::string password = read_terminal_line("password: ");
	WiFi.begin(ssid.c_str(), password.c_str());
#else
	WiFi.begin("www.vanheusden.com", "Ditiseentest31415926");
	//WiFi.begin("NURDspace-guest", "harkharkhark");
#endif

	while (WiFi.status() != WL_CONNECTED) {
		Serial.print('.');

		delay(250);
	}

	on_wifi = true;

	Serial.println(WiFi.localIP());
#endif
}

void setup() {
	Serial.begin(115200);

	Serial.println(F("This PDP-11 emulator is called \"kek\" (reason for that is forgotten) and was written by Folkert van Heusden."));

	Serial.println(F("Build on: " __DATE__ " " __TIME__));

	Serial.print(F("Size of int: "));
	Serial.println(sizeof(int));

	Serial.print(F("CPU clock frequency (MHz): "));
	Serial.println(getCpuFrequencyMhz());

	Serial.print(F("Free RAM before init (decimal bytes): "));
	Serial.println(ESP.getFreeHeap());

	Serial.println(F("Init bus"));
	b = new bus();

	Serial.println(F("Init CPU"));
	c = new cpu(b, &event);

	Serial.println(F("Connect CPU to BUS"));
	b->add_cpu(c);

	c->setEmulateMFPT(true);

	Serial.println(F("Init console"));
	cnsl = new console_esp32(&terminate);

	Serial.println(F("Init TTY"));
	tty_ = new tty(cnsl);
	Serial.println(F("Connect TTY to bus"));
	b->add_tty(tty_);

	Serial.print(F("Starting panel (on CPU 0, main emulator runs on CPU "));
	Serial.print(xPortGetCoreID());
	Serial.println(F(")"));
	xTaskCreatePinnedToCore(&panel, "panel", 2048, b, 1, nullptr, 0);

	// setup_wifi_stations();

	Serial.println(F("Load RK05"));
	b->add_rk05(new rk05("", b, &disk_read_activity, &disk_write_activity));
	setBootLoader(b);

	Serial.print(F("Free RAM after init: "));
	Serial.println(ESP.getFreeHeap());

	pinMode(LED_BUILTIN, OUTPUT);

	Serial.flush();

	Serial.println(F("Press <enter> to start"));

	for(;;) {
		if (Serial.available()) {
			int c = Serial.read();
			if (c == 13 || c == 10)
				break;
		}

		delay(1);
	}

	Serial.println(F("Emulation starting!"));

	start_ts = millis();

	running = true;
}

uint32_t icount = 0;

void dump_state(bus *const b) {
	cpu *const c = b->getCpu();

	uint32_t now = millis();
	uint32_t t_diff = now - start_ts;

	double mips = icount / (1000.0 * t_diff);

	// see https://retrocomputing.stackexchange.com/questions/6960/what-was-the-clock-speed-and-ips-for-the-original-pdp-11
	constexpr double pdp11_clock_cycle = 150;  // ns, for the 11/70
	constexpr double pdp11_mhz = 1000.0 / pdp11_clock_cycle; 
	constexpr double pdp11_avg_cycles_per_instruction = (1 + 5) / 2.0;
	constexpr double pdp11_estimated_mips = pdp11_mhz / pdp11_avg_cycles_per_instruction;

	Serial.print(F("MIPS: "));
	Serial.println(mips);

	Serial.print(F("emulation speed (aproximately): "));
	Serial.print(mips * 100 / pdp11_estimated_mips);
	Serial.println('%');

	Serial.print(F("PC: "));
	Serial.println(c->getPC());

	Serial.print(F("Uptime (ms): "));
	Serial.println(t_diff);
}

bool poll_char()
{
	return Serial.available() > 0;
}

char get_char()
{
	char c = Serial.read();

	if (c == 5)
		dump_state(b);

	return c;
}

void put_char(char c)
{
	Serial.print(c);
}

void loop() {
	icount++;

	c->step();

	if (event || terminate) {
		running = false;

		Serial.println(F(""));
		Serial.println(F(" *** EMULATION STOPPED *** "));
		dump_state(b);
		delay(3000);
		Serial.println(F(" *** EMULATION RESTARTING *** "));

		c->reset();
		c->setRegister(7, exec_addr);

		start_ts = millis();
		icount = 0;

		terminate = false;
		event     = 0;

		running   = true;
	}
}
