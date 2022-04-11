// (C) 2018-2022 by Folkert van Heusden
// Released under Apache License v2.0
#pragma once

extern bool trace_output;

#if defined(ESP32)
#define D(...) do { } while(0);
#else
#ifndef NDEBUG
#define D(x) do { if (trace_output) { x } } while(0);
#else
#define D(...) do { } while(0);
#endif
#endif
