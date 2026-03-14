# This file is included by CMakeLists.txt and pulls in the Pico SDK.
if (DEFINED ENV{PICO_SDK_PATH} AND EXISTS "$ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
elseif (EXISTS "${CMAKE_CURRENT_LIST_DIR}/pico-sdk/external/pico_sdk_import.cmake")
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk")
else()
    message(FATAL_ERROR "PICO_SDK_PATH is not set (or invalid) and pico-sdk submodule not found")
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
