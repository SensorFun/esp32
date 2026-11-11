set(requires esp_driver_gpio esp_system nvs_flash freertos esp_timer)
# P4 has no integrated Wi-Fi. A project can still use common lifecycle/reset
# handling without linking the built-in-radio provisioning implementation.
if(NOT IDF_TARGET STREQUAL "esp32p4")
    list(APPEND requires provisioning)
endif()
idf_component_register(SRCS "application.cpp" INCLUDE_DIRS "include" REQUIRES ${requires})
