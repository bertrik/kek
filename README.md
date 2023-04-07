KEK
Kek is a DEC PDP-11 (11/70) emulator capable of running UNIX-v6.

To build for e.g. linux:

    mkdir build
    cd build
    cmake ..
    make

    Required:
    * libncursesw5-dev

To build for e.g. windows:

    mkdir build-win32
    cd build-win32
    cmake -DCMAKE_TOOLCHAIN_FILE=../mingw64.cmake ..
    make


To run a disk image:

    ./kek -R filename.rk -b 2> /dev/null

Kek emulates an RK05.


To run a tape image:

    ./kek -T filename.bin -b 2> /dev/null


The ESP32 version needs platformio to be build.

    cd ESP32
    pio run -t upload

That should build & upload it to a connected ESP32.

Wiring of SDCARD (or use disk-images exported via NBD over wifi using nbdkit (because of older NBD protocol implementation)):
* MISO: 19
* MOSI: 23
* SCK : 18
* SS  : 5
* and of course connect VCC/GND of the SD-card

Wiring of the MAX232 connection:
* TX  : 17
* RX  : 16


See SHA2017-badge.md if you want to flash your SHA20127 badge with this software.


Some pictures:

![(running on a regular ESP32 connected to a VT510 terminal)](images/KEK-ESP32-VT510.jpg)

![(running on a SHA2017-badge)](images/KEK-sha2017badge.jpg)



Released under MIT license.

Folkert van Heusden
