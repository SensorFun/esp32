/* Signaling-only Janus plugin: SDP is opaque application data, never JSEP.
 * WebRTC stays end-to-end between camera and viewer, including its data channel.
 */
#include <plugins/plugin.h>
#include <jansson.h>
#include <string.h>

typedef struct {
    janus_plugin_session *handle;
    char room[33];
    int role; /* 0: camera, 1: viewer, -1: not joined */
} member;
typedef struct { member *members[2]; } room;
static janus_callbacks *gateway;
static GHashTable *rooms;
static GMutex mutex;
static janus_plugin plugin;

static int init(janus_callbacks *callbacks, const char *path) {
    (void)path;
    gateway = callbacks;
    rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return 0;
}
static void destroy(void) { g_hash_table_destroy(rooms); rooms = NULL; }
static int api(void) { return JANUS_PLUGIN_API_VERSION; }
static int version(void) { return 1; }
static const char *version_string(void) { return "0.1.0"; }
static const char *description(void) { return "Two-peer signaling without media relay"; }
static const char *name(void) { return "Camera signaling"; }
static const char *author(void) { return "ESP32 workspace contributors"; }
static const char *package(void) { return "janus.plugin.signaling"; }
static void noop(janus_plugin_session *handle) { (void)handle; }
static json_t *query(janus_plugin_session *handle) { (void)handle; return json_object(); }
static json_t *admin_message(json_t *message) { (void)message; return json_object(); }
static void create_session(janus_plugin_session *handle, int *error) {
    member *item = g_new0(member, 1);
    item->handle = handle;
    item->role = -1;
    handle->plugin_handle = item;
    *error = 0;
}
/* Caller holds mutex. Keep a Janus reference across unlock and push_event. */
static janus_plugin_session *pin(member *item) {
    if (!item || g_atomic_int_get(&item->handle->stopped)) return NULL;
    janus_refcount_increase(&item->handle->ref);
    return item->handle;
}
static void deliver(janus_plugin_session *recipient, json_t *message) {
    if (!recipient) return;
    json_t *event = json_pack("{s:s,s:O}", "event", "signal", "message", message);
    gateway->push_event(recipient, &plugin, NULL, event, NULL);
    json_decref(event);
    janus_refcount_decrease(&recipient->ref);
}
static void destroy_session(janus_plugin_session *handle, int *error) {
    janus_plugin_session *other = NULL;
    g_mutex_lock(&mutex);
    member *item = handle->plugin_handle;
    if (item && item->role >= 0) {
        room *pair = g_hash_table_lookup(rooms, item->room);
        if (pair) {
            other = pin(pair->members[1 - item->role]);
            pair->members[item->role] = NULL;
            if (!pair->members[0] && !pair->members[1]) g_hash_table_remove(rooms, item->room);
        }
    }
    handle->plugin_handle = NULL;
    g_free(item);
    g_mutex_unlock(&mutex);
    json_t *bye = json_pack("{s:s}", "type", "bye");
    deliver(other, bye);
    json_decref(bye);
    *error = 0;
}
static gboolean valid_room(const char *value) {
    if (!value || strlen(value) != 32) return FALSE;
    return strspn(value, "0123456789ABCDEF") == 32;
}
static gboolean valid_signal(json_t *message, int role) {
    if (!json_is_object(message)) return FALSE;
    const char *type = json_string_value(json_object_get(message, "type"));
    if (!type) return FALSE;
    if (!strcmp(type, "offer") || !strcmp(type, "answer")) {
        json_t *sdp = json_object_get(message, "sdp");
        return role == (!strcmp(type, "offer") ? 1 : 0) &&
            json_is_string(sdp) && json_string_length(sdp) <= 24576;
    }
    if (!strcmp(type, "candidate")) {
        json_t *candidate = json_object_get(message, "candidate");
        return json_is_string(candidate) && json_string_length(candidate) <= 8192;
    }
    if (!strcmp(type, "customized")) {
        json_t *data = json_object_get(message, "data");
        return json_is_string(data) && json_string_length(data) <= 8192;
    }
    return !strcmp(type, "bye");
}
static janus_plugin_result *handle_message(janus_plugin_session *handle,
        char *transaction, json_t *message, json_t *jsep) {
    const char *failure = NULL;
    const char *event = "sent";
    janus_plugin_session *recipient = NULL;
    json_t *signal = NULL;
    const char *request = json_string_value(json_object_get(message, "request"));
    g_mutex_lock(&mutex);
    member *item = handle->plugin_handle;
    if (!item || !request || jsep) {
        failure = "Invalid signaling request; JSEP is not supported";
    } else if (!strcmp(request, "join")) {
        const char *key = json_string_value(json_object_get(message, "room"));
        const char *role_name = json_string_value(json_object_get(message, "role"));
        int role = role_name && !strcmp(role_name, "camera") ? 0 :
            role_name && !strcmp(role_name, "viewer") ? 1 : -1;
        if (item->role >= 0 || !valid_room(key) || role < 0) {
            failure = "Invalid room membership";
        } else {
            room *pair = g_hash_table_lookup(rooms, key);
            if (role == 1 && (!pair || !pair->members[0])) failure = "Camera not found; check MAC/password and camera connection";
            else if (pair && pair->members[role]) failure = "Camera or viewer is already connected";
            else if (!pair && g_hash_table_size(rooms) >= 128) failure = "Signaling server is full";
            else {
                if (!pair) {
                    pair = g_new0(room, 1);
                    g_hash_table_insert(rooms, g_strdup(key), pair);
                }
                item->role = role;
                memcpy(item->room, key, 33);
                pair->members[role] = item;
                event = "joined";
                if (pair->members[0] && pair->members[1]) {
                    recipient = pin(pair->members[0]);
                    signal = json_pack("{s:s,s:s}", "type", "customized", "data", "connect");
                }
            }
        }
    } else if (!strcmp(request, "send") && item->role >= 0) {
        json_t *payload = json_object_get(message, "message");
        if (!valid_signal(payload, item->role) || json_dumpb(payload, NULL, 0, JSON_COMPACT) > 32768) {
            failure = "Invalid signal";
        } else {
            room *pair = g_hash_table_lookup(rooms, item->room);
            recipient = pair ? pin(pair->members[1 - item->role]) : NULL;
            /* Dropping a bye for a departed peer is harmless. Other messages
             * require a peer, so failed negotiation is visible to the caller. */
            if (!recipient && strcmp(json_string_value(json_object_get(payload, "type")), "bye"))
                failure = "Other peer is not connected";
            else signal = json_incref(payload);
        }
    } else failure = "Join a room before sending signals";
    g_mutex_unlock(&mutex);
    json_t *response = failure ? json_pack("{s:s}", "error", failure) : json_pack("{s:s}", "event", event);
    /* Queue membership confirmation before a camera's initial connect event.
     * This ordering matters when the camera is the second peer to join. */
    gboolean joined = !failure && !strcmp(event, "joined");
    if (joined) gateway->push_event(handle, &plugin, transaction, response, NULL);
    if (signal) { deliver(recipient, signal); json_decref(signal); }
    g_free(transaction);
    json_decref(message);
    json_decref(jsep);
    if (joined) {
        json_decref(response);
        return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, NULL, NULL);
    }
    return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, response);
}
static janus_plugin plugin = {
    .init = init, .destroy = destroy, .get_api_compatibility = api,
    .get_version = version, .get_version_string = version_string,
    .get_description = description, .get_name = name, .get_author = author,
    .get_package = package, .create_session = create_session,
    .handle_message = handle_message, .handle_admin_message = admin_message,
    .setup_media = noop, .hangup_media = noop, .data_ready = noop,
    .destroy_session = destroy_session, .query_session = query,
};
janus_plugin *create(void) { return &plugin; }
