cmake_minimum_required(VERSION 3.16)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/board.cmake")
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/main/generate_profiles.py"
    "${WORKSPACE_ROOT}" "${CMAKE_BINARY_DIR}/camera_profiles.h" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WORKSPACE_ROOT}/config/frame_grabber/ov2640.yaml"
    "${WORKSPACE_ROOT}/config/frame_grabber/ov3660.yaml"
    "${CMAKE_CURRENT_LIST_DIR}/main/generate_profiles.py")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(webrtc)
