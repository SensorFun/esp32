"""Exercise the firmware event buffer with the real JSON implementation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CJSON = ROOT / 'projects/webrtc/managed_components/espressif__cjson/cJSON'


@unittest.skipUnless(shutil.which('g++') and (CJSON / 'cJSON.c').exists(), 'Requires g++ and firmware cJSON dependency')
class DiagnosticsTests(unittest.TestCase):
    def test_history_bounds_cursors_and_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'sdkconfig.h').write_text('#pragma once\n#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH 0\n')
            (folder / 'freertos').mkdir()
            (folder / 'freertos/FreeRTOS.h').write_text('#pragma once\nusing portMUX_TYPE = int;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n')
            (folder / 'esp_timer.h').write_text('#pragma once\n#include <cstdint>\ninline int64_t esp_timer_get_time() { static int64_t t=0; return t+=1000; }\n')
            (folder / 'test.cpp').write_text(r'''
#include "diagnostics.h"
#include <cassert>
#include <string>
int main() {
    auto *empty = diagnostic_logs(0);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(empty, "entries")) == 0);
    cJSON_Delete(empty);
    for (int i=0;i<60;++i) diagnostic_event("info", ("event " + std::to_string(i)).c_str());
    auto *reply = diagnostic_logs(1);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(reply,"gap")));
    auto *entries = cJSON_GetObjectItem(reply,"entries");
    assert(cJSON_GetArraySize(entries)==4);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(entries,0),"id")->valueint==13);
    cJSON_Delete(reply);
    reply=diagnostic_logs(60);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(reply,"entries"))==0);
    cJSON_Delete(reply);
    diagnostic_event("error", std::string(500,'x').c_str());
    reply=diagnostic_logs(60);
    auto *entry=cJSON_GetArrayItem(cJSON_GetObjectItem(reply,"entries"),0);
    assert(std::string(cJSON_GetObjectItem(entry,"message")->valuestring).size()==127);
    cJSON_Delete(reply);

    // Clearing hides history without reporting a gap or resetting ids.
    const uint32_t cleared = diagnostic_clear();
    assert(cleared == 61);
    for (uint32_t cursor : {0u, 1u, 61u}) {
        reply = diagnostic_logs(cursor);
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(reply,"entries"))==0);
        assert(!cJSON_IsTrue(cJSON_GetObjectItem(reply,"gap")));
        cJSON_Delete(reply);
    }
    // A repeat of the last cleared message is a new entry, not a hidden merge.
    diagnostic_event("error", std::string(500,'x').c_str());
    diagnostic_event("info", "after clear");
    reply = diagnostic_logs(5);
    entries = cJSON_GetObjectItem(reply,"entries");
    assert(cJSON_GetArraySize(entries)==2);
    assert(!cJSON_IsTrue(cJSON_GetObjectItem(reply,"gap")));
    entry = cJSON_GetArrayItem(entries,0);
    assert(cJSON_GetObjectItem(entry,"id")->valueint==62);
    assert(cJSON_GetObjectItem(entry,"repeats")->valueint==1);
    assert(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(entries,1),"message")->valuestring)=="after clear");
    cJSON_Delete(reply);

    // Overflow after a clear is still reported as a gap.
    for (int i=0;i<60;++i) diagnostic_event("info", ("later " + std::to_string(i)).c_str());
    reply = diagnostic_logs(63);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(reply,"gap")));
    cJSON_Delete(reply);
}
''')
            binary = folder / 'test'
            subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', f'-I{folder}', f'-I{CJSON}', f'-I{ROOT / "projects/webrtc/main"}', str(folder / 'test.cpp'), str(ROOT / 'projects/webrtc/main/diagnostics.cpp'), str(CJSON / 'cJSON.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
