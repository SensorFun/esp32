if(IDF_TARGET STREQUAL "esp32p4")
    # P4 has no built-in radio; common application setup skips provisioning.
    idf_component_register(INCLUDE_DIRS "include")
    return()
endif()
idf_component_register(
    SRCS "provisioning.cpp"
    INCLUDE_DIRS "include"
    # local_http.h, shared with the camera viewer, includes esp_http_server.h.
    REQUIRES camera_access esp_app_format tls_identity esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_hw_support
)
