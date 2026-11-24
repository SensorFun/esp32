#include "motion_detector.h"

#include <array>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    MotionSettings settings;
    settings.enabled = true;  // This runner exercises detection, not factory enablement.
    if (argc < 2) return 2;
    const int64_t work_us = std::stoll(argv[1]);
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const float value = std::stof(argv[i + 1]);
        if (key == "enabled") settings.enabled = value != 0;
        else if (key == "cell") settings.cell = int(value);
        else if (key == "blur") settings.blur = int(value);
        else if (key == "min_speed") settings.min_speed = value;
        else if (key == "min_texture") settings.min_texture = value;
        else if (key == "min_area") settings.min_area = value;
        else if (key == "trigger_seconds") settings.trigger_seconds = value;
        else return 2;
    }

    std::array<uint8_t, motion::kMaxPixels> previous{}, current{};
    motion::JpegScratch scratch;
    motion::Detector detector(previous.data(), current.data(), scratch);
    motion::Activity activity;
    std::vector<uint8_t> jpeg;
    int64_t captured, busy_until = 0, prior = -1;
    size_t size;
    std::cout << std::boolalpha;
    // Each input entry is a timestamp/length line followed by exactly that many JPEG bytes.
    while (std::cin >> captured >> size) {
        if (captured <= prior || !size || std::cin.get() != '\n') return 2;
        prior = captured;
        jpeg.resize(size);
        if (!std::cin.read(reinterpret_cast<char *>(jpeg.data()), size)) return 2;
        // Like the firmware: no rate cap, each finished analysis takes the next frame.
        if (!settings.enabled || captured < busy_until) continue;
        busy_until = captured + work_us;
        const auto result = detector.analyze(jpeg.data(), jpeg.size(), captured, settings);
        const bool analyzed = result == motion::FrameResult::analyzed;
        if (analyzed) activity.update(detector.flow.motion, busy_until);
        std::cout << "{\"capture_us\":" << captured << ",\"complete_us\":" << busy_until
                  << ",\"result\":\"" << (analyzed ? "analyzed" : result == motion::FrameResult::priming ? "priming" : "invalid")
                  << "\",\"width\":" << detector.analysis_width << ",\"height\":" << detector.analysis_height
                  << ",\"interval_s\":" << detector.interval << ",\"motion\":" << (analyzed && detector.flow.motion)
                  << ",\"area\":" << (analyzed ? detector.flow.area : 0)
                  << ",\"speed\":" << (analyzed ? detector.speed : 0)
                  << ",\"held\":" << activity.moving
                  << ",\"trigger\":" << (analyzed && activity.confirmed(busy_until, settings.trigger_seconds))
                  << ",\"cols\":" << (analyzed ? detector.flow.cols : 0)
                  << ",\"rows\":" << (analyzed ? detector.flow.rows : 0)
                  << ",\"grid\":\"";
        if (analyzed) for (int i = 0; i < detector.flow.cols * detector.flow.rows; ++i)
            std::cout << (detector.flow.moving[i] ? '1' : '0');
        std::cout << "\"}\n";
    }
    return std::cin.eof() ? 0 : 2;
}
