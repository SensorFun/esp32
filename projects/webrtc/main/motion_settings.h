#pragma once

// Motion detection and SD recording settings. The firmware's field table is
// the single source of truth: viewers render whatever motion_command returns.
struct MotionSettings {
    bool enabled = false;
    int method = 0;              // Index into the detection method list.
    float min_speed = 2.5f;      // Analysis-image pixels per second, independent of the analysis rate.
    float min_area = 2.0f;       // Percent of cells.
    int cell = 8;
    float min_texture = 4.0f;
    int blur = 1;
    float trigger_seconds = 1.0f;
    bool overlay = true;
    bool record = true;
    int record_fps = 10;
    int post_seconds = 10;
    int max_clip_seconds = 120;
    bool overwrite_oldest = true;
};
