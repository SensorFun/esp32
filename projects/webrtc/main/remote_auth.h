#pragma once
#include "camera_access.h"
#include "cJSON.h"
#include <string>

class RemoteAuth {
public:
    static bool configured();
    static bool blocked();
    void clear();
    cJSON *begin(const std::string &offer, const std::string &answer);
    cJSON *verify(const cJSON *request);
    bool allowed() const;
    bool expired(int64_t now) const;
private:
    camera_access::Record record_{};
    std::string transcript_;
    bool authenticated_ = false;
    uint32_t generation_ = 0;
    int64_t deadline_ = 0;
};
