cmake_minimum_required(VERSION 3.22)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

include(pico_sdk_import.cmake)

project(sputter_control_panel C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
file(GLOB SCP_MODULE_MAIN_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/modules/*/src/main.c
)

if(NOT DEFINED SCP_MODULE_TARGET)
    set(SCP_MODULE_TARGET roughing_pump)
endif()

option(SCP_BUILD_ALL_MODULES "Build firmware images for all modules" OFF)

pico_sdk_init()

add_subdirectory(bsp/pico)
add_subdirectory(shared)

if(SCP_BUILD_ALL_MODULES OR SCP_MODULE_TARGET STREQUAL "pirani")
    add_subdirectory(lvgl)
    target_include_directories(lvgl SYSTEM PUBLIC
        ${CMAKE_SOURCE_DIR}
    )
endif()

add_custom_target(check_module_pins
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_module_pins.py --generate-html --docs-dir docs/pinout --root ${CMAKE_SOURCE_DIR} --quiet
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/check_module_pins.py ${SCP_MODULE_MAIN_SOURCES}
    COMMENT "Checking module GPIO assignments for collisions"
    VERBATIM
)

add_subdirectory(modules)
