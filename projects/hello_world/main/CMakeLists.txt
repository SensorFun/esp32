idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES freertos log provisioning)
target_compile_options(${COMPONENT_LIB} PRIVATE "-include" "${BOARD_HEADER}")
