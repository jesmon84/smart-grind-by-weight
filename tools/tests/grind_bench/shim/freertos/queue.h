#pragma once

#include <cstddef>
#include <cstring>
#include <deque>
#include <vector>

#include "FreeRTOS.h"

// Bounded copy-in, copy-out queue with the FreeRTOS calls the controller uses.
struct BenchQueue {
    size_t capacity;
    size_t item_size;
    std::deque<std::vector<unsigned char>> items;
};
using QueueHandle_t = BenchQueue*;

inline QueueHandle_t xQueueCreate(size_t capacity, size_t item_size) {
    return new BenchQueue{capacity, item_size, {}};
}

inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t) {
    if (!queue || queue->items.size() >= queue->capacity) return 0;
    const auto* bytes = static_cast<const unsigned char*>(item);
    queue->items.emplace_back(bytes, bytes + queue->item_size);
    return pdPASS;
}

inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t) {
    if (!queue || queue->items.empty()) return 0;
    std::memcpy(item, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    return pdPASS;
}
