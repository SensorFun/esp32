#include "recorder.h"
#include "avi_writer.h"
#include "sd_card.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr char TAG[] = "recorder";
constexpr char kFolder[] = "/sdcard/REC";
constexpr int kPage = 25;
constexpr int64_t kSecond = 1000000;
#if CONFIG_IDF_TARGET_ESP32
constexpr size_t kStaging = 8 * 1024;  // Internal RAM is scarce on the original ESP32.
#else
constexpr size_t kStaging = 16 * 1024;
#endif

struct Frame {
    uint8_t *data;
    uint32_t length;
    uint16_t width, height;
    int64_t time;
};

QueueHandle_t queue;
SemaphoreHandle_t sd_lock;   // Serializes mount changes with list/delete/open.
SemaphoreHandle_t state_lock;
std::atomic<size_t> buffered{0};
size_t budget = 0;
int64_t last_accept = 0;
// Shared state, guarded by state_lock.
std::string active_clip, last_error, format_result;
// Set by viewers, handled by the recorder task, which owns mounting.
std::atomic<bool> format_requested{false};
int64_t clip_started = 0;
uint32_t clip_frames = 0;

struct Guard {
    explicit Guard(SemaphoreHandle_t handle) : handle_(handle) { xSemaphoreTakeRecursive(handle_, portMAX_DELAY); }
    ~Guard() { xSemaphoreGiveRecursive(handle_); }
    SemaphoreHandle_t handle_;
};

void release(Frame &frame) {
    heap_caps_free(frame.data);
    buffered -= frame.length;
    frame.data = nullptr;
}

void set_error(const char *message) {
    Guard guard(state_lock);
    last_error = message ? message : "";
}

bool digits(const char *text, int count) {
    for (int i = 0; i < count; ++i) if (!isdigit(static_cast<unsigned char>(text[i]))) return false;
    return true;
}
bool is_day(const char *name) { return strlen(name) == 8 && digits(name, 8); }
bool is_clip(const char *name) {
    const size_t length = strlen(name);
    if (length != 10 && length != 11) return false;
    if (!digits(name, 6)) return false;
    if (length == 11 && !(name[6] >= 'A' && name[6] <= 'Z')) return false;
    return !strcasecmp(name + length - 4, ".AVI");
}
// Names viewers may request: YYYYMMDD/HHMMSS[A-Z].AVI
bool valid_name(const char *name) {
    return name && strlen(name) <= 20 && name[8] == '/' && digits(name, 8) && is_clip(name + 9);
}

std::vector<std::string> sorted_entries(const char *path, bool days) {
    std::vector<std::string> names;
    if (DIR *dir = opendir(path)) {
        while (dirent *entry = readdir(dir)) {
            if (days ? is_day(entry->d_name) : is_clip(entry->d_name)) names.emplace_back(entry->d_name);
        }
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// All clips, oldest first, as "YYYYMMDD/HHMMSS.AVI".
std::vector<std::string> all_clips() {
    std::vector<std::string> clips;
    for (const auto &day : sorted_entries(kFolder, true)) {
        const std::string folder = std::string(kFolder) + "/" + day;
        for (const auto &clip : sorted_entries(folder.c_str(), false)) clips.push_back(day + "/" + clip);
    }
    return clips;
}

bool remove_clip(const std::string &name) {
    const std::string path = std::string(kFolder) + "/" + name;
    if (unlink(path.c_str()) != 0) return false;
    rmdir((std::string(kFolder) + "/" + name.substr(0, 8)).c_str());  // Only succeeds once empty.
    return true;
}

// Keeps at least 1/50 of the card (64 MB minimum) free, deleting the oldest
// finished clips when allowed. Returns false when the card stays full.
bool ensure_space(bool overwrite) {
    uint64_t total = 0, free = 0;
    if (!sd_card::space(total, free)) return false;
    const uint64_t reserve = std::max<uint64_t>(64ull << 20, total / 50);
    if (free >= reserve) return true;
    if (!overwrite) return false;
    std::string active;
    { Guard guard(state_lock); active = active_clip; }
    for (const auto &clip : all_clips()) {
        if (clip == active) continue;
        if (remove_clip(clip)) ESP_LOGI(TAG, "Card full: deleted oldest clip %s", clip.c_str());
        if (!sd_card::space(total, free) || free >= reserve) return free >= reserve;
    }
    return false;
}

class Clip {
public:
    bool open(const Frame &first, bool overwrite) {
        Guard sd(sd_lock);
        if (!ensure_space(overwrite)) { set_error(overwrite ? "SD card full" : "SD card full; recording paused"); return false; }
        staging_ = static_cast<uint8_t *>(heap_caps_malloc(kStaging, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!staging_) { set_error("Not enough memory to record"); return false; }
        // Name the clip after its first frame's wall-clock time (UTC).
        const time_t start = time(nullptr) - time_t((esp_timer_get_time() - first.time) / kSecond);
        tm utc{};
        gmtime_r(&start, &utc);
        char day[16], path[48];
        strftime(day, sizeof(day), "%Y%m%d", &utc);
        mkdir(kFolder, 0775);
        snprintf(path, sizeof(path), "%s/%s", kFolder, day);
        mkdir(path, 0775);
        char clock[8];
        strftime(clock, sizeof(clock), "%H%M%S", &utc);
        struct stat info;
        name_.clear();
        for (char suffix = 0; suffix <= 'Z'; suffix = suffix ? suffix + 1 : 'A') {
            snprintf(path, sizeof(path), "%s/%s/%s%.1s.AVI", kFolder, day, clock, suffix ? &suffix : "");
            if (stat(path, &info) != 0) { name_ = path + strlen(kFolder) + 1; break; }
        }
        if (name_.empty() || !avi_.open(path, first.width, first.height, staging_, kStaging)) {
            heap_caps_free(staging_); staging_ = nullptr;
            set_error("Cannot create a file on the SD card");
            return false;
        }
        first_ = last_ = first.time;
        last_sync_ = last_space_check_ = esp_timer_get_time();
        {
            Guard guard(state_lock);
            active_clip = name_; clip_started = first.time; clip_frames = 0; last_error.clear();
        }
        ESP_LOGI(TAG, "Recording %s at %ux%u", name_.c_str(), first.width, first.height);
        return true;
    }
    bool is_open() const { return avi_.is_open(); }
    bool matches(const Frame &frame) const { return frame.width == avi_.width() && frame.height == avi_.height(); }
    int64_t started() const { return first_; }
    bool write(const Frame &frame, bool overwrite) {
        if (!avi_.add_frame(frame.data, frame.length)) return false;
        last_ = frame.time;
        { Guard guard(state_lock); clip_frames = avi_.frames(); }
        const int64_t now = esp_timer_get_time();
        // Make data durable regularly, so a power cut loses only seconds.
        if (now - last_sync_ > 3 * kSecond) {
            last_sync_ = now;
            if (!avi_.sync()) return false;
        }
        if (now - last_space_check_ > 10 * kSecond) {
            last_space_check_ = now;
            Guard sd(sd_lock);
            if (!ensure_space(overwrite)) { set_error("SD card full"); close(); }
        }
        return true;
    }
    void close() {
        if (!avi_.is_open()) return;
        const uint32_t frames = avi_.frames();
        // Spread the measured span over the frame count; frame times mark starts.
        const uint64_t span = frames > 1 ? uint64_t(last_ - first_) * frames / (frames - 1) : kSecond / 10;
        const bool ok = avi_.close(span);
        ESP_LOGI(TAG, "Closed %s: %lu frames, %.1f s%s", name_.c_str(), (unsigned long)frames, span / 1e6, ok ? "" : " (write error)");
        finish();
    }
    void abort() {
        avi_.abort();
        finish();
    }

private:
    void finish() {
        heap_caps_free(staging_); staging_ = nullptr;
        Guard guard(state_lock);
        active_clip.clear(); clip_frames = 0;
    }
    motion::AviWriter avi_;
    uint8_t *staging_ = nullptr;
    std::string name_;
    int64_t first_ = 0, last_ = 0, last_sync_ = 0, last_space_check_ = 0;
};

void record(void *) {
    Clip clip;
    std::deque<Frame> pending;  // Pre-roll while idle; frames to write while recording.
    int64_t next_mount = 0, retry_open = 0;
    for (;;) {
        Frame frame;
        const bool received = xQueueReceive(queue, &frame, pdMS_TO_TICKS(200)) == pdTRUE;
        if (received) pending.push_back(frame);
        const int64_t now = esp_timer_get_time();
        const MotionSettings s = motion_settings();
        if (format_requested) {
            // Close files first; frames captured meanwhile are dropped.
            clip.close();
            for (auto &item : pending) release(item);
            pending.clear();
            esp_err_t err;
            {
                Guard sd(sd_lock);
                err = sd_card::format();
            }
            {
                Guard guard(state_lock);
                format_result = err == ESP_OK ? "SD card formatted" : std::string("Format failed: ") + esp_err_to_name(err);
                if (err == ESP_OK) last_error.clear();
            }
            format_requested = false;
            next_mount = esp_timer_get_time() + 10 * kSecond;
            continue;
        }
        const sd_card::State state = sd_card::state();
        if (state != sd_card::State::mounted && state != sd_card::State::unsupported && state != sd_card::State::formatting && now >= next_mount) {
            Guard sd(sd_lock);
            sd_card::mount();
            next_mount = now + 10 * kSecond;
        }
        const bool wanted = s.enabled && s.record && sd_card::state() == sd_card::State::mounted;
        if (!wanted) {
            clip.close();
            for (auto &item : pending) release(item);
            pending.clear();
            continue;
        }
        bool moving;
        int64_t since, last;
        motion_activity(moving, since, last);
        if (!clip.is_open()) {
            // Keep the pre-roll window plus the confirmation time, so a clip
            // starts before the motion that triggered it.
            // The pre-record length never exceeds what PSRAM holds at the
            // current resolution, even if it changed since it was saved.
            const int pre = std::min(s.pre_seconds, motion_pre_seconds_limit(s));
            const int64_t keep = int64_t((pre + s.trigger_seconds + 0.5f) * kSecond);
            size_t held = 0;
            for (const auto &item : pending) held += item.length;
            while (!pending.empty() && (now - pending.front().time > keep || held > budget * 4 / 5)) {
                held -= pending.front().length;
                release(pending.front()); pending.pop_front();
            }
            if (!moving || now - since < int64_t(s.trigger_seconds * kSecond) || pending.empty() || now < retry_open) continue;
            if (!clip.open(pending.front(), s.overwrite_oldest)) {
                retry_open = now + 10 * kSecond;  // A full card is checked again later, not per frame.
                continue;
            }
        }
        while (!pending.empty()) {
            Frame &item = pending.front();
            if (!clip.matches(item)) {
                // A resolution change continues in a new clip at the new size.
                clip.close();
                if (!clip.open(item, s.overwrite_oldest)) break;
            }
            if (!clip.write(item, s.overwrite_oldest)) {
                ESP_LOGE(TAG, "SD write failed; unmounting card");
                clip.abort();
                set_error("SD card write failed");
                Guard sd(sd_lock);
                sd_card::unmount(sd_card::State::error);
                break;
            }
            release(item);
            pending.pop_front();
            if (!clip.is_open()) break;  // Closed for lack of space.
        }
        if (clip.is_open() && ((!moving && now - last > int64_t(s.post_seconds) * kSecond) ||
                               now - clip.started() > int64_t(s.max_clip_seconds) * kSecond)) {
            clip.close();  // Continuing motion opens the next clip immediately.
        }
        if (!clip.is_open()) {
            // Frames left after a failed open or write are dropped.
            if (sd_card::state() != sd_card::State::mounted) {
                for (auto &item : pending) release(item);
                pending.clear();
            }
        }
    }
}

void add_card(cJSON *object) {
    cJSON *card = cJSON_AddObjectToObject(object, "sd");
    cJSON_AddStringToObject(card, "state", sd_card::state_name());
    uint64_t total = 0, free = 0;
    if (sd_card::space(total, free)) {
        cJSON_AddNumberToObject(card, "total_mb", double(total >> 20));
        cJSON_AddNumberToObject(card, "free_mb", double(free >> 20));
    }
}
}  // namespace

void recorder_start() {
    queue = xQueueCreate(48, sizeof(Frame));
    sd_lock = xSemaphoreCreateRecursiveMutex();
    state_lock = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(queue && sd_lock && state_lock ? ESP_OK : ESP_ERR_NO_MEM);
    budget = std::clamp<size_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 4, 512 * 1024, 3 * 1024 * 1024);
    {
        Guard sd(sd_lock);
        sd_card::mount();  // Optional: without a card, motion is signalled but not recorded.
    }
    if (xTaskCreatePinnedToCore(record, "recorder", 6144, nullptr, 4, nullptr, 1) != pdPASS) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
}

size_t recorder_budget() { return budget; }

bool recorder_frame_due(const MotionSettings &s, int64_t now) {
    return s.record && sd_card::state() == sd_card::State::mounted && buffered < budget &&
           now - last_accept >= kSecond / s.record_fps;
}

void recorder_feed(const camera_fb_t *frame, const MotionSettings &s) {
    const int64_t now = esp_timer_get_time();
    if (!recorder_frame_due(s, now) || buffered + frame->len > budget) return;
    Frame copy{static_cast<uint8_t *>(heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), uint32_t(frame->len),
               uint16_t(frame->width), uint16_t(frame->height), now};
    if (!copy.data) return;
    memcpy(copy.data, frame->buf, frame->len);
    buffered += copy.length;
    last_accept = now;
    if (xQueueSend(queue, &copy, 0) != pdTRUE) release(copy);
}

void recorder_status(cJSON *status) {
    {
        Guard guard(state_lock);
        cJSON_AddBoolToObject(status, "recording", !active_clip.empty());
        if (!active_clip.empty()) {
            cJSON_AddStringToObject(status, "clip", active_clip.c_str());
            cJSON_AddNumberToObject(status, "clip_seconds", double((esp_timer_get_time() - clip_started) / kSecond));
            cJSON_AddNumberToObject(status, "clip_frames", clip_frames);
        }
        if (!last_error.empty()) cJSON_AddStringToObject(status, "recorder_error", last_error.c_str());
        if (!format_result.empty()) cJSON_AddStringToObject(status, "format_result", format_result.c_str());
    }
    add_card(status);
    cJSON_AddBoolToObject(status, "clock", time(nullptr) > 1700000000);
}

const char *recorder_request_format() {
    const sd_card::State state = sd_card::state();
    if (state == sd_card::State::unsupported) return "This board has no SD card slot";
    if (state == sd_card::State::formatting || format_requested) return "The SD card is already being formatted";
    if (state == sd_card::State::absent) return "No SD card inserted";
    { Guard guard(state_lock); format_result.clear(); }
    format_requested = true;
    ESP_LOGW(TAG, "SD card format requested by a viewer");
    return nullptr;
}

cJSON *recorder_list(int offset) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "recordings");
    cJSON *files = cJSON_AddArrayToObject(reply, "files");
    if (format_requested || sd_card::state() == sd_card::State::formatting) {
        add_card(reply);
        cJSON_AddNumberToObject(reply, "total", 0);
        cJSON_AddNumberToObject(reply, "offset", 0);
        cJSON_AddNumberToObject(reply, "page", kPage);
        return reply;
    }
    Guard sd(sd_lock);
    add_card(reply);
    std::vector<std::string> clips;
    if (sd_card::state() == sd_card::State::mounted) clips = all_clips();
    std::reverse(clips.begin(), clips.end());
    std::string active;
    { Guard guard(state_lock); active = active_clip; }
    cJSON_AddNumberToObject(reply, "total", double(clips.size()));
    cJSON_AddNumberToObject(reply, "offset", offset);
    cJSON_AddNumberToObject(reply, "page", kPage);
    for (size_t i = size_t(offset); i < clips.size() && i < size_t(offset) + kPage; ++i) {
        const std::string path = std::string(kFolder) + "/" + clips[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", clips[i].c_str());
        struct stat info;
        if (stat(path.c_str(), &info) == 0) cJSON_AddNumberToObject(item, "size", double(info.st_size));
        if (clips[i] == active) cJSON_AddBoolToObject(item, "recording", true);
        else if (int fd = open(path.c_str(), O_RDONLY); fd >= 0) {
            motion::AviInfo avi;
            if (motion::read_avi_info(fd, avi)) {
                cJSON_AddNumberToObject(item, "frames", avi.frames);
                cJSON_AddNumberToObject(item, "duration", avi.finalized ? double(uint64_t(avi.frames) * avi.micro_per_frame / 1000) / 1000 : 0);
                cJSON_AddNumberToObject(item, "width", avi.width);
                cJSON_AddNumberToObject(item, "height", avi.height);
                cJSON_AddBoolToObject(item, "complete", avi.finalized);
            }
            close(fd);
        }
        cJSON_AddItemToArray(files, item);
    }
    return reply;
}

const char *recorder_delete(const char *name) {
    if (!valid_name(name)) return "Invalid recording name";
    if (format_requested || sd_card::state() == sd_card::State::formatting) return "The SD card is being formatted";
    Guard sd(sd_lock);
    if (sd_card::state() != sd_card::State::mounted) return "No SD card";
    { Guard guard(state_lock); if (active_clip == name) return "This clip is still recording"; }
    return remove_clip(name) ? nullptr : "Recording not found";
}

const char *recorder_open(const char *name, int &fd, uint32_t &size) {
    if (!valid_name(name)) return "Invalid recording name";
    if (format_requested || sd_card::state() == sd_card::State::formatting) return "The SD card is being formatted";
    Guard sd(sd_lock);
    if (sd_card::state() != sd_card::State::mounted) return "No SD card";
    { Guard guard(state_lock); if (active_clip == name) return "This clip is still recording"; }
    const std::string path = std::string(kFolder) + "/" + name;
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || info.st_size <= 0) return "Recording not found";
    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "Cannot open recording";
    size = uint32_t(info.st_size);
    return nullptr;
}
