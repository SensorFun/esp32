# Include before ESP-IDF project.cmake in each firmware project.
get_filename_component(WORKSPACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(BOARD "esp32s3_seeed_cam" CACHE STRING "Board configuration name")
if(NOT BOARD MATCHES "^[A-Za-z0-9_]+$")
    message(FATAL_ERROR "Invalid board name: ${BOARD}")
endif()
set(BOARD_CONFIG "${WORKSPACE_ROOT}/config/board/${BOARD}.yaml")
if(NOT EXISTS "${BOARD_CONFIG}")
    message(FATAL_ERROR "Unknown board: ${BOARD}")
endif()
# CMake has no YAML reader. Resolve the Hydra board profile using the project's
# Python dependency; JSON is only an in-memory interchange format here.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${WORKSPACE_ROOT}/python"
        "${Python3_EXECUTABLE}" -m esp32_workspace.boards "${WORKSPACE_ROOT}" "${BOARD}"
    RESULT_VARIABLE BOARD_RESULT
    OUTPUT_VARIABLE BOARD_JSON
    ERROR_VARIABLE BOARD_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT BOARD_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot read board profile: ${BOARD_ERROR}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${BOARD_CONFIG}" "${WORKSPACE_ROOT}/python/esp32_workspace/boards.py")
string(JSON BOARD_TARGET GET "${BOARD_JSON}" target)
if(DEFINED IDF_TARGET AND NOT IDF_TARGET STREQUAL BOARD_TARGET)
    message(FATAL_ERROR "Board ${BOARD} requires ${BOARD_TARGET}, got ${IDF_TARGET}")
endif()
set(IDF_TARGET "${BOARD_TARGET}" CACHE STRING "ESP-IDF target")
set(BOARD_HEADER "${WORKSPACE_ROOT}/boards/${BOARD}.h")

string(JSON DEFAULT_CAMERA ERROR_VARIABLE CAMERA_ERROR GET "${BOARD_JSON}" default_camera)
if(CAMERA_ERROR)
    set(DEFAULT_CAMERA "none")
endif()
set(CAMERA_SENSOR "${DEFAULT_CAMERA}" CACHE STRING "Camera sensor profile")
if(NOT CAMERA_SENSOR MATCHES "^(ov[0-9]+|none)$")
    message(FATAL_ERROR "Invalid camera sensor: ${CAMERA_SENSOR}")
endif()
if(NOT CAMERA_SENSOR STREQUAL "none")
    string(JSON CAMERA_COUNT LENGTH "${BOARD_JSON}" camera_sensors)
    set(CAMERA_FOUND FALSE)
    if(CAMERA_COUNT GREATER 0)
        math(EXPR LAST_CAMERA "${CAMERA_COUNT} - 1")
        foreach(INDEX RANGE ${LAST_CAMERA})
            string(JSON SUPPORTED_CAMERA GET "${BOARD_JSON}" camera_sensors ${INDEX})
            if(SUPPORTED_CAMERA STREQUAL CAMERA_SENSOR)
                set(CAMERA_FOUND TRUE)
            endif()
        endforeach()
    endif()
    if(NOT CAMERA_FOUND)
        message(FATAL_ERROR "Camera ${CAMERA_SENSOR} is not supported by ${BOARD}")
    endif()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/firmware_selection.json"
    "{\"board\":\"${BOARD}\",\"camera\":\"${CAMERA_SENSOR}\"}\n")
