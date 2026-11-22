#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

// In-memory stand-in for the ESP32 Preferences store.
class Preferences {
public:
    std::map<std::string, double> values;

    int getInt(const char* key, int fallback = 0) const { return static_cast<int>(get(key, fallback)); }
    float getFloat(const char* key, float fallback = 0.0f) const { return static_cast<float>(get(key, fallback)); }
    uint64_t getULong64(const char* key, uint64_t fallback = 0) const {
        return static_cast<uint64_t>(get(key, static_cast<double>(fallback)));
    }
    size_t putInt(const char* key, int value) { values[key] = value; return sizeof(int); }
    size_t putFloat(const char* key, float value) { values[key] = value; return sizeof(float); }
    size_t putULong64(const char* key, uint64_t value) { values[key] = static_cast<double>(value); return sizeof(uint64_t); }

private:
    double get(const char* key, double fallback) const {
        const auto found = values.find(key);
        return found == values.end() ? fallback : found->second;
    }
};
