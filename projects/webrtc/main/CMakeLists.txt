idf_component_register(SRCS "main.cpp" "camera_controls.cpp" INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}"
    REQUIRES application provisioning tls_identity esp_https_server esp_peer esp32-camera cjson esp_timer esp_wifi media_lib_sal
    EMBED_TXTFILES "viewer.html")
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-include${BOARD_HEADER}>")
