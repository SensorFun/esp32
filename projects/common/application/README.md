# Common application lifecycle

Every firmware project can use `common::Application` for shared setup, reset
inputs, NVS initialization, and Wi-Fi provisioning. Board headers provide
`common_board_config`; projects provide their own nonblocking application work.

Add `projects/common` to `EXTRA_COMPONENT_DIRS` before ESP-IDF's `project.cmake`,
and add `application` to the main component's `REQUIRES`:

```cpp
#include "common/application.h"

extern "C" void app_main(void) {
    static common::Application common(common_board_config);
    ESP_ERROR_CHECK(common.setup());
    while (true) {
        common.main();
        // Project work goes here; avoid long blocking operations.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
```

Include FreeRTOS headers for the delay, as Hello World does. Keep the instance
alive for the application's lifetime. `setup()` runs once before project
peripherals claim pins. Call `main()` at least every 50 ms. Any project can call
`common.enter_setup()` to clear persistent settings and restart provisioning.
Passing `false` as the constructor's second argument disables the optional serial
`wifi-reset` command if another application owns stdin.

## Board reset inputs

`common::SetupReset` defaults to BOOT/GPIO0, a two-second hold, runtime-only
checking, and an upward restart pull. `BoardConfig` uses these defaults and enables
Wi-Fi unless overridden. Board headers only specify exceptions: ESP32-CAM uses
GPIO12/startup checking/downward restart pull; P4 uses GPIO35/five seconds and
has no built-in Wi-Fi.

Available overrides are:

- `sense_gpio`: input with an internal pull-up; `-1` disables physical reset.
- `bridge_gpio`: `-1` means connect the sense pin to GND; otherwise bridge the
  sense GPIO to this second GPIO. Pair detection probes the second pin open-drain
  and never drives it high. A sense pin grounded independently does not count as
  a bridged pair.
- `hold_ms`: continuous hold duration, followed by 100 ms of release debounce.
- `check_at_startup`: also check for a jumper before networking starts. Keep this
  false for BOOT pins that select the ROM download loader when held at power-on.
- `restart_pull`: restore a board-appropriate strapping pull before restarting.

| Board | Trigger | Hold | Startup check |
| --- | --- | --- | --- |
| AI Thinker ESP32-CAM | GPIO12 / IO12 to GND | 2 seconds, then disconnect | Yes |
| ESP32-S3 WROOM camera | BOOT / GPIO0 | 2 seconds, then release | Runtime only |
| Seeed XIAO ESP32-S3 | BOOT / GPIO0 | 2 seconds, then release | Runtime only |
| Waveshare ESP32-P4 | BOOT / GPIO35 | 5 seconds, then release | Runtime only |

For a pair of GPIOs, configure `.sense_gpio = <input>` and
`.bridge_gpio = <open-drain-capable output>` using verified, free board pins.
For a button or jumper to ground, use `.bridge_gpio = -1`.

On the ESP32-CAM, IO12 is also the microSD D2 line and a flash-voltage strapping
pin. This firmware reserves it for reset, enables its sensing pull-up only after
boot, and restores its low pull before restart. Do not connect it to 3.3 V or use
it simultaneously for SD. U0T is UART TX, not the reset input. If a future project
needs SD or another peripheral on a reset pin, disable or reassign the trigger
in its board configuration before enabling that peripheral.

## NVS reset

The trigger arms after the hold duration and acts only after release. This avoids
reboot loops with a jumper still connected or booting GPIO0 boards into download
mode. A short press does not erase anything.

`enter_setup()` records an explicit request in RTC no-init memory and restarts.
On the next software boot, `setup()` erases the **entire default NVS partition**
before Wi-Fi or project tasks open storage handles. This includes saved Wi-Fi
credentials and other application settings stored there. It then initializes NVS
and starts fresh provisioning. Other flash partitions are untouched. Ordinary
power cycles, reset presses, and panic resets do not request NVS erasure.

ESP32/ESP32-S3 use the shared provisioning component. P4 can use lifecycle and
reset handling but has no integrated Wi-Fi; external-radio provisioning remains
unsupported here.
