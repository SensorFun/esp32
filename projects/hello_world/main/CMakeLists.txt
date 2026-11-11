idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES freertos log application)
target_compile_options(${COMPONENT_LIB} PRIVATE "-include" "${BOARD_HEADER}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE FIRMWARE_CAMERA_SENSOR="${CAMERA_SENSOR}")
