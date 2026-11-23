#pragma once
#include "esp_err.h"
#include <cstdint>

// microSD over SDMMC in one-bit mode, using the board header's SD_MMC_* pins.
// Boards without them report "unsupported". Only the recorder task mounts or
// unmounts; status functions are safe from any task.
namespace sd_card {

enum class State { unsupported, absent, mounted, error, formatting };

constexpr const char *kRoot = "/sdcard";

esp_err_t mount();
// Erases the card and creates a new FAT volume, also on a card that has none.
// Call only from the task that owns mounting, with no files open.
esp_err_t format();
void unmount(State reason);
// Asks a mounted card for its status. A card that no longer answers was
// removed or reseated: it is unmounted as absent so the next mount
// initializes it again. Call only from the task that owns mounting.
bool check();
State state();
const char *state_name();
// Returns false when unmounted or the volume cannot report its size.
bool space(uint64_t &total, uint64_t &free);

}  // namespace sd_card
