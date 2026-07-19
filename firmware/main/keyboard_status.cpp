#include "keyboard_status.hpp"

#include <atomic>

static std::atomic<bool> s_connected{false};

void keyboard_status_set(bool connected)
{
    s_connected.store(connected, std::memory_order_relaxed);
}

bool keyboard_status_get()
{
    return s_connected.load(std::memory_order_relaxed);
}
