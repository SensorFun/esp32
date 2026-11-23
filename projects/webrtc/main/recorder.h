#pragma once
#include "cJSON.h"
#include "esp_camera.h"
#include "motion.h"
#include <cstdint>

// MJPEG AVI clips on the SD card, stored as
// REC/YYYYMMDD/HHMMSS.AVI (UTC; 8.3 names work without FAT long-name support).
void recorder_start();
// PSRAM bytes reserved for buffered frames (pre-roll plus write queue).
size_t recorder_budget();
// Whether the recorder would accept a frame now (recording or pre-roll).
bool recorder_frame_due(const MotionSettings &settings, int64_t now);
// Copies an accepted JPEG frame for the recorder task.
void recorder_feed(const camera_fb_t *frame, const MotionSettings &settings);
enum class ManualRecording { idle, active, stopping };
ManualRecording recorder_manual_state();
const char *recorder_set_manual(bool start);
bool recorder_manual_frame_due();
void recorder_manual_feed(const camera_fb_t *frame);
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

// Finalizes current clips and discards queued frames before acknowledging.
void recorder_pause(bool paused);
