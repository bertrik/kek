// (C) 2018-2022 by Folkert van Heusden
// Released under Apache License v2.0
#include <FastLED.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <WiFi.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "cpu.h"
#include "error.h"
#include "esp32.h"
#include "memory.h"
#include "tty.h"
#include "utils.h"


#define NEOPIXELS_PIN	25
bus *b    = nullptr;
cpu *c    = nullptr;
tty *tty_ = nullptr;

uint32_t event     = 0;

uint16_t exec_addr = 0;

uint32_t start_ts  = 0;

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

	CRGB leds[32] { 0 };
	FastLED.addLeds<NEOPIXEL, NEOPIXELS_PIN>(leds, 32);

	FastLED.setBrightness(50);

	FastLED.show();

	const CRGB run_mode_led_color[4] = { CRGB::Red, CRGB::Yellow, CRGB::Blue, CRGB::Green };

	for(;;) {
		vTaskDelay(20 / portTICK_RATE_MS);

		uint16_t current_PC      = c->getPC();
		uint32_t full_addr       = b->calculate_full_address(current_PC);

		uint16_t current_PSW     = c->getPSW();

		const CRGB & led_color   = run_mode_led_color[current_PSW >> 14];

		for(int b=0; b<22; b++)
			leds[b] = full_addr & (1 << b) ? led_color : CRGB::Black;

		leds[22] = c->getPSW_c() ? CRGB::Magenta : CRGB::Black;
		leds[23] = c->getPSW_v() ? CRGB::Magenta : CRGB::Black;
		leds[24] = c->getPSW_z() ? CRGB::Magenta : CRGB::Black;
		leds[25] = c->getPSW_n() ? CRGB::Magenta : CRGB::Black;

		FastLED.show();
	}
}

SemaphoreHandle_t terminal_mutex = xSemaphoreCreateMutex();

constexpr int terminal_columns = 80;
constexpr int terminal_rows    = 25;
char terminal[terminal_columns * terminal_rows];
uint8_t tx = 0, ty = terminal_rows - 1;
QueueHandle_t to_telnet_queue = xQueueCreate(10, sizeof(char));

void delete_first_line() {
	memmove(&terminal[0], &terminal[terminal_columns], terminal_columns * (terminal_rows - 1));
	memset(&terminal[terminal_columns * (terminal_rows - 1)], ' ', terminal_columns);
}

void telnet_terminal(void *p) {
	bus *const b = reinterpret_cast<bus *>(p);
	tty *const tty_ = b->getTty();

	Serial.println(F("telnet_terminal task started"));

	if (!tty_)
		Serial.println(F(" *** NO TTY ***"));

	for(;;) {
		char cc { 0 };

		xQueueReceive(tty_->getTerminalQueue(), &cc, portMAX_DELAY);

		Serial.print(cc);

		// update terminal buffer
		xSemaphoreTake(terminal_mutex, portMAX_DELAY);

		if (cc == 13)
			tx = 0;
		else if (cc == 10)
			ty++;
		else {
			terminal[ty * terminal_columns + tx] = cc;

			tx++;

			if (tx == terminal_columns)
				tx = 0, ty++;
		}

		if (ty == terminal_rows) {
			delete_first_line();
			ty--;
		}

		xSemaphoreGive(terminal_mutex);

		// pass through to telnet clients
		if (xQueueSend(to_telnet_queue, &cc, portMAX_DELAY) != pdTRUE)
			Serial.println(F("queue TTY character failed"));
	}
}

void wifi(void *p) {
	Serial.println(F("wifi task started"));

	int fd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in server { 0 };
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(23);
 
	if (bind(fd, (struct sockaddr *)&server, sizeof(server)) == -1)
		Serial.println(F("bind failed"));

	if (listen(fd, 3) == -1)
		Serial.println(F("listen failed"));

	struct pollfd fds[] = { { fd, POLLIN, 0 } };

	std::vector<int> clients;

	for(;;) {
		int rc = poll(fds, 1, 10);

		if (rc == 1) {
			int client = accept(fd, nullptr, nullptr);
			if (client != -1) {
				clients.push_back(client);

				constexpr const uint8_t dont_auth[] = { 0xff, 0xf4, 0x25,  // don't auth
									0xff, 0xfb, 0x03,  // suppress goahead
									0xff, 0xfe, 0x22,  // don't line-mode
									0xff, 0xfe, 0x27,  // don't new envt0
									0xff, 0xfb, 0x01,  // will echo
									0xff, 0xfe, 0x01,  // don't echo
									0xff, 0xfd, 0x2d };  // no echo

				write(client, dont_auth, sizeof(dont_auth));

				// send initial terminal stat
				write(client, "\033[2J", 4);

				xSemaphoreTake(terminal_mutex, portMAX_DELAY);

				for(int y=0; y<terminal_rows; y++) {
					std::string out = format("\033[%dH", y + 1);
					if (write(client, out.c_str(), out.size()) != out.size())
						break;

					if (write(client, &terminal[y * terminal_columns], terminal_columns) != terminal_columns)
						break;
				}

				xSemaphoreGive(terminal_mutex);
			}
		}

		std::string out;
		char c { 0 };
		while (xQueueReceive(to_telnet_queue, &c, 10 / portMAX_DELAY) == pdTRUE)
			out += c;

		if (!out.empty()) {
			for(size_t i=0; i<clients.size();) {
				if (write(clients.at(i), out.c_str(), out.size()) == -1) {
					close(clients.at(i));
					clients.erase(clients.begin() + i);
				}
				else {
					i++;
				}
			}
		}
	}
}

void setup_wifi_stations()
{
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

	Serial.println(WiFi.localIP());
}

void setup() {
	Serial.begin(115200);

	Serial.println(F("This PDP-11 emulator is called \"kek\" (reason for that is forgotten) and was written by Folkert van Heusden."));

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

	Serial.println(F("Init TTY"));
	tty_ = new tty(poll_char, get_char, put_char);
	Serial.println(F("Connect TTY to bus"));
	b->add_tty(tty_);

	Serial.print(F("Starting panel (on CPU 0, main emulator runs on CPU "));
	Serial.print(xPortGetCoreID());
	Serial.println(F(")"));
	xTaskCreatePinnedToCore(&panel, "panel", 2048, b, 1, nullptr, 0);

	memset(terminal, ' ', sizeof(terminal));
	xTaskCreatePinnedToCore(&telnet_terminal, "telnet", 2048, b, 7, nullptr, 0);

	xTaskCreatePinnedToCore(&wifi, "wifi", 2048, b, 7, nullptr, 0);

	setup_wifi_stations();

	Serial.println(F("Load RK05"));
	b->add_rk05(new rk05("", b));
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

	if (event) {
		Serial.println(F(""));
		Serial.println(F(" *** EMULATION STOPPED *** "));
		dump_state(b);
		delay(3000);
		Serial.println(F(" *** EMULATION RESTARTING *** "));

		c->setRegister(7, exec_addr);
		c->resetHalt();

		start_ts = millis();
		icount = 0;
	}
}
