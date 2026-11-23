idf_component_register(SRCS "diagnostics.cpp" "main.cpp" "camera_controls.cpp" "camera_frames.cpp" "remote_signaling.cpp" "camera_signaling.cpp" "remote_auth.cpp" "peer_receive.c" "peer_send.c" "camera_poll.c" "optical_flow.cpp" "motion_detector.cpp" "motion_jpeg.c" "avi_writer.cpp" "motion.cpp" "recorder.cpp" "sd_card.cpp" INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}"
    REQUIRES spi_flash espcoredump camera_access apprtc_signal application provisioning tls_identity esp_https_server esp_peer esp32-camera cjson esp_new_jpeg esp_timer esp_wifi media_lib_sal esp_websocket_client mbedtls nvs_flash esp_jpeg fatfs sdmmc esp_driver_sdmmc esp_netif
    EMBED_TXTFILES "viewer.html" "../../common/camera_ui/settings.js" "../../common/camera_ui/motion.js" "../../common/camera_ui/logs.js")
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-include${BOARD_HEADER}>")
if(DEFINED ENV{CAMERA_SIGNALING_API_SECRET})
    set(signaling_object "${CMAKE_BINARY_DIR}/ci_camera_signaling.o")
    set(signaling_compiler "${CMAKE_CURRENT_LIST_DIR}/compile_signaling_credential.py")
    add_custom_command(OUTPUT "${signaling_object}"
        COMMAND "${Python3_EXECUTABLE}" "${signaling_compiler}" "${CMAKE_C_COMPILER}" "${signaling_object}"
        DEPENDS "${signaling_compiler}" VERBATIM)
    set_source_files_properties("${signaling_object}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    target_sources(${COMPONENT_LIB} PRIVATE "${signaling_object}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE USE_CI_CAMERA_SIGNALING)
endif()
# The transport shim targets the exact esp_peer revision pinned in the manifest.
idf_component_get_property(peer_dir esp_peer COMPONENT_DIR)
idf_component_get_property(camera_dir espressif__esp32-camera COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${peer_dir}/src/transport" "${camera_dir}/driver/private_include")
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=udp_socket_recvfrom_nowait" "-Wl,--wrap=udp_socket_sendto" "-Wl,-u,__wrap_udp_socket_sendto" "-Wl,--wrap=cam_take" "-Wl,-u,__wrap_cam_take")

idf_component_get_property(jpeg_dir espressif__esp_jpeg COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${jpeg_dir}/tjpgd")
