idf_component_register(SRCS "signal_default.c" "https_client.c" "esp_peer_signaling.c"
    INCLUDE_DIRS "include" "." REQUIRES nvs_flash esp_http_client esp_websocket_client esp_peer cjson media_lib_sal)
