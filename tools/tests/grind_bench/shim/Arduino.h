#pragma once

// Desktop stand-in for the Arduino core, for the grind bench only. Time is
// the bench's own clock, so a whole grind runs in milliseconds.
#include <alloca.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using std::max;
using std::min;

unsigned long millis();
int64_t esp_timer_get_time();

// Everything the firmware logs ends up here for the scenarios to read.
extern std::string bench_log;

struct BenchSerial {
    template <typename... Args>
    void printf(const char* format, Args... args) const {
        char line[256];
        std::snprintf(line, sizeof(line), format, args...);
        bench_log += line;
    }
    void println(const char* message) const {
        bench_log += message;
        bench_log += '\n';
    }
};

inline BenchSerial Serial;
