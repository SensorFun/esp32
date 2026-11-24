#pragma once

// Motion detection and SD recording settings. The firmware's field table is
// the single source of truth: viewers render whatever motion_command returns.
struct MotionSettings {
    bool enabled = false;
    int method = 0;              // Index into the detection method list.
    // Defaults tuned on tests/data/motion/camera-2026-11-23_14-59-34.avi: a
    // person crossing the street is detected while swaying palm fronds and
    // trees are not, at the clip's 6.5 fps and at half that. Minimum speed has
    // the widest margin (6-10 px/s all stay clean); 1.5% area or texture 8 let
    // background through.
    float min_speed = 8.0f;      // Analysis-image pixels per second, independent of the analysis rate.
    float min_area = 2.0f;       // Percent of cells.
    int cell = 10;
    float min_texture = 12.0f;
    int blur = 2;
    float trigger_seconds = 1.0f;
    bool overlay = true;
    bool record = true;
    int post_seconds = 10;
    int max_clip_seconds = 120;
    bool overwrite_oldest = true;
};
