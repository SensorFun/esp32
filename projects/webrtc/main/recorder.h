#pragma once
#include "cJSON.h"
#include "esp_camera.h"
#include "motion.h"
#include <cstdint>

// Motion-triggered MJPEG AVI clips on the SD card, stored as
// REC/YYYYMMDD/HHMMSS.AVI (UTC; 8.3 names work without FAT long-name support).
void recorder_start();
// PSRAM bytes reserved for buffered frames (pre-roll plus write queue).
size_t recorder_budget();
// Whether the recorder would accept a frame now (recording or pre-roll).
bool recorder_frame_due(const MotionSettings &settings, int64_t now);
// Copies an accepted JPEG frame for the recorder task.
void recorder_feed(const camera_fb_t *frame, const MotionSettings &settings);
// One page of clips, newest first.
cJSON *recorder_list(int offset);
// Returns an error message, or nullptr once deleted.
const char *recorder_delete(const char *name);
// Opens a finished clip for reading. Returns an error message or nullptr.
const char *recorder_open(const char *name, int &fd, uint32_t &size);
// Queues a card format for the recorder task. Returns an error message or nullptr.
const char *recorder_request_format();
// Adds recording and SD card fields to a motion status message.
void recorder_status(cJSON *status);
