#include "remote_auth.h"
#include "auth_protocol.h"
#include "esp_random.h"
#include "esp_timer.h"

namespace {
unsigned failures = 0;
int64_t blocked_until = 0;
cJSON *error(const char *message) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "auth_error");
    cJSON_AddStringToObject(reply, "error", message);
    return reply;
}
}
bool RemoteAuth::configured() {
    camera_access::Record record;
    bool ok = camera_access::load(record) == ESP_OK;
    camera_access::wipe(&record, sizeof(record)); return ok;
}
bool RemoteAuth::blocked() { return esp_timer_get_time() < blocked_until; }
void RemoteAuth::clear() { authenticated_ = false; deadline_ = 0; transcript_.clear(); camera_access::wipe(&record_, sizeof(record_)); }
bool RemoteAuth::allowed() const { return authenticated_ && generation_ == camera_access::generation(); }
bool RemoteAuth::expired(int64_t now) const { return authenticated_ ? !allowed() : deadline_ > 0 && now >= deadline_; }
cJSON *RemoteAuth::begin(const std::string &offer, const std::string &answer) {
    clear();
    deadline_ = esp_timer_get_time() + 15000000;
    if (blocked() || camera_access::load(record_, &generation_) != ESP_OK) return error("Remote camera access is unavailable");
    const std::string offer_fp = access_protocol::fingerprint(offer), answer_fp = access_protocol::fingerprint(answer);
    char room[32];
    if (offer_fp.empty() || answer_fp.empty() || camera_access::mac_room(room, sizeof(room)) != ESP_OK) return error("Cannot bind authentication to this WebRTC connection");
    uint8_t bytes[32]; esp_fill_random(bytes, sizeof(bytes));
    const std::string nonce = access_protocol::hex(bytes, sizeof(bytes));
    const std::string salt = access_protocol::hex(record_.salt, sizeof(record_.salt));
    transcript_ = access_protocol::transcript(room, salt, record_.iterations, nonce, offer_fp, answer_fp);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "auth_challenge");
    cJSON_AddStringToObject(reply, "algorithm", "camera-scram-sha256-v2");
    cJSON_AddStringToObject(reply, "salt", salt.c_str()); cJSON_AddStringToObject(reply, "nonce", nonce.c_str());
    cJSON_AddNumberToObject(reply, "iterations", record_.iterations);
    return reply;
}
cJSON *RemoteAuth::verify(const cJSON *request) {
    if (blocked() || expired(esp_timer_get_time()) || transcript_.empty() || generation_ != camera_access::generation()) return error("Authentication expired; reconnect");
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    const cJSON *proof_json = cJSON_GetObjectItemCaseSensitive(request, "proof");
    if (!cJSON_IsString(op) || std::string(op->valuestring) != "authenticate") return error("Camera password authentication required");
    uint8_t proof[32]{}, signature[32]{}, client_key[32]{}, stored_key[32]{};
    bool ok = cJSON_IsString(proof_json) && access_protocol::unhex(proof_json->valuestring, proof, 32) &&
        camera_access::mac(record_.stored_key, 32, transcript_.data(), transcript_.size(), signature);
    if (ok) {
        for (int i = 0; i < 32; ++i) client_key[i] = proof[i] ^ signature[i];
        ok = camera_access::hash(client_key, 32, stored_key) && camera_access::equal(stored_key, record_.stored_key, 32);
    }
    camera_access::wipe(proof, sizeof(proof)); camera_access::wipe(client_key, sizeof(client_key)); camera_access::wipe(stored_key, sizeof(stored_key));
    if (!ok) {
        camera_access::wipe(signature, sizeof(signature));
        if (++failures >= 5) { blocked_until = esp_timer_get_time() + 30000000; failures = 0; }
        return error("Camera authentication failed");
    }
    if (!camera_access::mac(record_.server_key, 32, transcript_.data(), transcript_.size(), signature)) return error("Authentication failed");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "auth_ok");
    cJSON_AddStringToObject(reply, "signature", access_protocol::hex(signature, 32).c_str());
    camera_access::wipe(signature, sizeof(signature));
    authenticated_ = true; failures = 0;
    return reply;
}
