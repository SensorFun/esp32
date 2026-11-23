#pragma once
#include <cstdint>
#include "cJSON.h"
// Only fixed application messages belong here: never network payloads or credentials.
void diagnostic_event(const char *level, const char *message);
cJSON *diagnostic_logs(uint32_t after);
// Hides every current entry from later reads; new events continue the sequence.
// Returns the last cleared id.
uint32_t diagnostic_clear();

void diagnostic_boot();

void diagnostic_eventf(const char *level, const char *format, ...) __attribute__((format(printf, 2, 3)));
