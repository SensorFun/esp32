#pragma once
#include <cstdint>
#include "cJSON.h"
// Only fixed application messages belong here: never network payloads or credentials.
void diagnostic_event(const char *level, const char *message);
cJSON *diagnostic_logs(uint32_t after);

void diagnostic_boot();
