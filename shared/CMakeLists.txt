add_library(scp_shared STATIC
    src/can_messages.c
    src/module_runtime.c
    src/pico_gpio_map.c
    src/protocol.c
)

target_include_directories(scp_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(scp_shared PUBLIC
    scp_bsp_pico
    pico_stdlib
)
