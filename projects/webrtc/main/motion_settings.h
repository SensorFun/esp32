#pragma once

// Motion detection and SD recording settings. Defaults and allowed values come
// from config/motion/motion.yaml (motion_config.h is generated from it at build
// time); viewers render whatever motion_command returns.
#include "motion_config.h"
struct MotionSettings {
    bool enabled = motion_config::enabled;
    int method = 0;              // Index into the detection method list.
    float min_speed = motion_config::min_speed;  // Analysis-image pixels per second, independent of the analysis rate.
    float min_area = motion_config::min_area;    // Percent of cells.
    int cell = motion_config::cell;
    float min_texture = motion_config::min_texture;
    int blur = motion_config::blur;
    float trigger_seconds = motion_config::trigger_seconds;
    bool overlay = motion_config::overlay;
    bool record = motion_config::record;
    int post_seconds = motion_config::post_seconds;
    int max_clip_seconds = motion_config::max_clip_seconds;
    bool overwrite_oldest = motion_config::overwrite_oldest;
};
