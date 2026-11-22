#pragma once

#include <cstdint>

// Single-threaded stand-ins: the bench calls the controller and the UI side in turn.
using BaseType_t = int;
using TickType_t = uint32_t;
constexpr BaseType_t pdPASS = 1;

struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}
