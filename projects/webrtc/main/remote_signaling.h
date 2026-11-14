#pragma once
#include <string>
#include "esp_peer.h"
struct RemoteEvent { int type; std::string data; };
void remote_start();
bool remote_receive(RemoteEvent &event);
void remote_send(int type, const char *data);
bool remote_connected();
void remote_ice(esp_peer_ice_server_cfg_t &server);
