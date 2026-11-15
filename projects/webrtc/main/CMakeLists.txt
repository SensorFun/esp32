idf_component_register(SRCS "main.cpp" "camera_controls.cpp" "remote_signaling.cpp" "camera_signaling.cpp" "remote_auth.cpp" "peer_receive.c" "peer_send.c" "camera_poll.c" INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}"
    REQUIRES camera_access apprtc_signal application provisioning tls_identity esp_https_server esp_peer esp32-camera cjson esp_timer esp_wifi media_lib_sal esp_websocket_client mbedtls nvs_flash
    EMBED_TXTFILES "viewer.html" "../../common/camera_ui/settings.js")
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-include${BOARD_HEADER}>")
# The transport shim targets the exact esp_peer revision pinned in the manifest.
idf_component_get_property(peer_dir esp_peer COMPONENT_DIR)
idf_component_get_property(camera_dir espressif__esp32-camera COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${peer_dir}/src/transport" "${camera_dir}/driver/private_include")
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=udp_socket_recvfrom_nowait" "-Wl,--wrap=udp_socket_sendto" "-Wl,-u,__wrap_udp_socket_sendto" "-Wl,--wrap=cam_take" "-Wl,-u,__wrap_cam_take")
