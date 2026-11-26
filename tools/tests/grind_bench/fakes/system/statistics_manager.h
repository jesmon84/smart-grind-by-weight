#pragma once

#include <cstdint>

class StatisticsManager {
public:
    void update_manual_grind(uint32_t) {}
    void update_time_pulse() {}
};

extern StatisticsManager statistics_manager;
