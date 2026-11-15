# Include before ESP-IDF project.cmake in each firmware project.
get_filename_component(WORKSPACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(BOARD "esp32_ai-thinker_cam" CACHE STRING "Board configuration name")
if(NOT BOARD MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "Invalid board name: ${BOARD}")
endif()
file(GLOB BOARD_CATALOGS CONFIGURE_DEPENDS "${WORKSPACE_ROOT}/config/boards/*.yaml")
set(BOARD_HEADER "${CMAKE_BINARY_DIR}/selected_board.h")
# CMake has no YAML reader. Resolve the Hydra board profile using the project's
# Python dependency; JSON is only an in-memory interchange format here.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${WORKSPACE_ROOT}/python"
        "${Python3_EXECUTABLE}" -m esp32_workspace.boards "${WORKSPACE_ROOT}" "${BOARD}" "${BOARD_HEADER}"
    RESULT_VARIABLE BOARD_RESULT
    OUTPUT_VARIABLE BOARD_JSON
    ERROR_VARIABLE BOARD_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT BOARD_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot read board profile: ${BOARD_ERROR}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${BOARD_CATALOGS} "${WORKSPACE_ROOT}/python/esp32_workspace/boards.py")
string(JSON BOARD_TARGET GET "${BOARD_JSON}" target)
if(DEFINED IDF_TARGET AND NOT IDF_TARGET STREQUAL BOARD_TARGET)
    message(FATAL_ERROR "Board ${BOARD} requires ${BOARD_TARGET}, got ${IDF_TARGET}")
endif()
set(IDF_TARGET "${BOARD_TARGET}" CACHE STRING "ESP-IDF target")
string(JSON BOARD_PINOUT GET "${BOARD_JSON}" header)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WORKSPACE_ROOT}/boards/${BOARD_PINOUT}")

set(FRAME_GRABBER "none" CACHE STRING "Frame grabber profile selected by the device")
if(NOT FRAME_GRABBER STREQUAL "none")
    string(JSON GRABBER_COUNT LENGTH "${BOARD_JSON}" supported_frame_grabber)
    set(GRABBER_FOUND FALSE)
    if(GRABBER_COUNT GREATER 0)
        math(EXPR LAST_GRABBER "${GRABBER_COUNT} - 1")
        foreach(INDEX RANGE ${LAST_GRABBER})
            string(JSON SUPPORTED_GRABBER GET "${BOARD_JSON}" supported_frame_grabber ${INDEX})
            if(SUPPORTED_GRABBER STREQUAL FRAME_GRABBER)
                set(GRABBER_FOUND TRUE)
            endif()
        endforeach()
    endif()
    if(NOT GRABBER_FOUND)
        message(FATAL_ERROR "Frame grabber ${FRAME_GRABBER} is not supported by ${BOARD}")
    endif()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/firmware_selection.json"
    "{\"board\":\"${BOARD}\",\"frame_grabber\":\"${FRAME_GRABBER}\"}\n")
