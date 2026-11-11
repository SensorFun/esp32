"""Check reset timing and deferred NVS erase without touching a real board."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "projects/common/application"


@unittest.skipUnless(shutil.which("c++"), "Native C++ compiler required")
class CommonApplicationTests(unittest.TestCase):
    def compile_and_run(self, source, stubs=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            if stubs:
                (root / "sdk_stubs.h").write_text(stubs)
                for name in ("driver/gpio.h", "esp_attr.h", "esp_check.h", "esp_err.h", "esp_log.h",
                             "esp_rom_sys.h", "esp_system.h", "esp_timer.h", "freertos/FreeRTOS.h",
                             "freertos/task.h", "nvs_flash.h", "soc/soc_caps.h"):
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text('#include "sdk_stubs.h"\n')
            (root / "test.cpp").write_text(source)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                            "-I", str(COMPONENT), "-I", str(COMPONENT / "include"), str(root / "test.cpp"),
                            "-o", str(root / "test")], check=True)
            subprocess.run([str(root / "test")], check=True)

    def test_two_second_hold_release_debounce_and_timer_wrap(self):
        self.compile_and_run(r'''
#include "common/reset_hold.h"
#include <cassert>
int main() {
    common::ResetHold reset(2000);
    assert(!reset.update(true, 0));
    assert(!reset.update(true, 1999));
    assert(!reset.armed());
    assert(!reset.update(true, 2000));
    assert(reset.armed());
    assert(!reset.update(false, 2001));
    assert(!reset.update(true, 2050)); // Release bounce cancels the debounce.
    assert(!reset.update(false, 2051));
    assert(!reset.update(false, 2150));
    assert(reset.update(false, 2151));
    assert(!reset.update(false, 2200)); // Exactly one trigger.
    assert(!reset.tracking());
    common::ResetHold interrupted(2000);
    interrupted.update(true, 0);
    interrupted.update(false, 500);
    interrupted.update(false, 600);
    interrupted.update(true, 1000);
    interrupted.update(true, 2999);
    assert(!interrupted.armed());
    interrupted.update(false, 3000);
    assert(!interrupted.update(false, 3100));
    common::ResetHold wrap(2000);
    wrap.update(true, UINT32_MAX - 1000);
    wrap.update(true, 998);
    assert(!wrap.armed());
    wrap.update(true, 999);
    assert(wrap.armed());
    wrap.update(false, 1000);
    assert(wrap.update(false, 1100));
}
''')

    def test_board_inputs_and_nvs_reset_lifecycle(self):
        stubs = r'''
#pragma once
#include <cstdint>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_INVALID_STATE=2, ESP_ERR_NOT_SUPPORTED=3, ESP_ERR_NO_MEM=4;
#define RTC_NOINIT_ATTR
#define SOC_WIFI_SUPPORTED 0
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { const int err=(expr); (void)(tag); if(err) return err; } while(0)
#define ESP_ERROR_CHECK(expr) do { if((expr)!=0) throw 1; } while(0)
#define ESP_LOGI(tag, ...) ((void)(tag))
using gpio_num_t=int;
constexpr int GPIO_MODE_INPUT=1,GPIO_MODE_OUTPUT_OD=2,GPIO_PULLUP_ENABLE=1,GPIO_PULLDOWN_ONLY=3,GPIO_PULLUP_ONLY=4;
#define GPIO_IS_VALID_GPIO(pin) ((pin)>=0 && (pin)<49)
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) GPIO_IS_VALID_GPIO(pin)
struct gpio_config_t { uint64_t pin_bit_mask; int mode; int pull_up_en; };
inline uint32_t clock_ms=0,release_at=UINT32_MAX;
inline bool grounded=false,bridged=false;
inline int levels[49]{},last_pull=0,erase_count=0,restart_count=0;
inline std::vector<char> nvs_operations;
inline int gpio_config(const gpio_config_t*) { return 0; }
inline int gpio_set_level(int pin,int level) { levels[pin]=level; return 0; }
inline int gpio_get_level(int pin) { return pin==12 ? ((grounded && clock_ms<release_at) || (bridged && levels[13]==0) ? 0 : 1) : levels[pin]; }
inline int gpio_set_pull_mode(int,int pull) { last_pull=pull; return 0; }
inline void esp_rom_delay_us(unsigned) {}
inline int64_t esp_timer_get_time() { return static_cast<int64_t>(clock_ms)*1000; }
enum ResetReason { ESP_RST_POWERON,ESP_RST_SW,ESP_RST_PANIC };
inline ResetReason reason=ESP_RST_POWERON;
inline ResetReason esp_reset_reason() { return reason; }
struct Reboot {};
inline void esp_restart() { ++restart_count; throw Reboot{}; }
inline int nvs_flash_erase() { ++erase_count; nvs_operations.push_back('E'); return 0; }
inline int nvs_flash_init() { nvs_operations.push_back('I'); return 0; }
constexpr int pdPASS=1;
#define pdMS_TO_TICKS(ms) (ms)
inline void vTaskDelay(unsigned ms) { clock_ms+=ms; }
inline int xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,void*) { return pdPASS; }
'''
        self.compile_and_run(r'''
#include "application.cpp"
#include <cassert>
common::BoardConfig board(int sense=12,int bridge=-1,bool startup=false) {
    return {"Example board", {sense,bridge,2000,startup,common::RestartPull::down}, false};
}
int main() {
    common::Application app(board(),false);
    app.enter_setup(); // Before setup, do not restart or erase.
    assert(restart_count==0 && erase_count==0);
    assert(app.setup()==ESP_OK);
    assert(app.setup()==ESP_ERR_INVALID_STATE);
    grounded=true;
    app.main();clock_ms=1999;app.main();assert(restart_count==0);
    clock_ms=2000;app.main();assert(restart_count==0);
    grounded=false;clock_ms=2001;app.main();clock_ms=2101;
    try { app.main();assert(false); } catch(const Reboot&) {}
    assert(restart_count==1 && erase_count==0); // Deferred until next boot.
    assert(last_pull==GPIO_PULLDOWN_ONLY);
    reason=ESP_RST_SW;
    common::Application rebooted(board(),false);
    assert(rebooted.setup()==ESP_OK);
    assert(erase_count==1);
    assert(nvs_operations[nvs_operations.size()-2]=='E' && nvs_operations.back()=='I');
    common::Application ordinary_reboot(board(),false);
    assert(ordinary_reboot.setup()==ESP_OK && erase_count==1);
    try { rebooted.enter_setup();assert(false); } catch(const Reboot&) {}
    reason=ESP_RST_PANIC;
    common::Application crashed(board(),false);
    assert(crashed.setup()==ESP_OK && erase_count==1); // Panic must not wipe data.
    reason=ESP_RST_POWERON;
    common::Application invalid(board(99),false);
    assert(invalid.setup()==ESP_ERR_INVALID_ARG);
    common::Application same_pins(board(12,12),false);
    assert(same_pins.setup()==ESP_ERR_INVALID_ARG);
    common::Application disabled(board(-1,-1),false);
    assert(disabled.setup()==ESP_OK);
    common::Application pair(board(12,13),false);
    assert(pair.setup()==ESP_OK);
    const int previous_restarts=restart_count;
    grounded=true;clock_ms=0;pair.main();clock_ms=2500;pair.main();
    grounded=false;clock_ms=2600;pair.main();clock_ms=2700;pair.main();
    assert(restart_count==previous_restarts); // A grounded sense pin is not a bridged pair.
    bridged=true;clock_ms=3000;pair.main();clock_ms=5000;pair.main();
    bridged=false;clock_ms=5001;pair.main();clock_ms=5101;
    try { pair.main();assert(false); } catch(const Reboot&) {}
    assert(restart_count==previous_restarts+1);
    // Consume the prior request, then verify a startup jumper can trigger reset.
    reason=ESP_RST_SW;
    common::Application clear_previous(board(),false);assert(clear_previous.setup()==ESP_OK);
    reason=ESP_RST_POWERON;clock_ms=0;grounded=true;release_at=2100;
    common::Application startup(board(12,-1,true),false);
    try { startup.setup();assert(false); } catch(const Reboot&) {}
    assert(clock_ms>=2200); // Must wait for release, not reboot with jumper connected.
}
''', stubs)
