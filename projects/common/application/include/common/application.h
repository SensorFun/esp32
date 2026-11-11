#pragma once
#include "common/board.h"
#include "common/reset_hold.h"
#include "esp_err.h"

namespace common {
// Application-lifetime object. setup() runs before project peripherals are started;
// main() must run at least every 50 ms, alongside nonblocking project work.
class Application {
public:
    explicit Application(BoardConfig board, bool serial_reset = true)
        : board_(board), reset_(board.setup_reset.hold_ms), serial_reset_(serial_reset) {}
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    esp_err_t setup();
    void main();
    // Reboot, erase the default NVS partition early on the next boot, then set up.
    // Includes Wi-Fi credentials AND other settings stored in default NVS.
    void enter_setup();
    const BoardConfig &board() const { return board_; }
private:
    esp_err_t configure_reset_pins();
    bool reset_active() const;
    static void serial_worker(void *context);
    BoardConfig board_;
    ResetHold reset_;
    bool serial_reset_, started_ = false, announced_ = false;
};
}
