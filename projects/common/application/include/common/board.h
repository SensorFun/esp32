#pragma once
#include <cstdint>

namespace common {
enum class RestartPull { unchanged, up, down };

struct SetupReset {
    int sense_gpio = 0;       // Pulled-up input; -1 disables the physical trigger.
    int bridge_gpio = -1;      // -1: sense-to-GND; otherwise bridge these two GPIOs.
    uint32_t hold_ms = 2000;
    bool check_at_startup = false; // False for BOOT strapping pins; runtime checks remain.
    RestartPull restart_pull = RestartPull::up;
};

struct BoardConfig {
    const char *name;
    SetupReset setup_reset{};
    bool has_wifi = true;
};
}
