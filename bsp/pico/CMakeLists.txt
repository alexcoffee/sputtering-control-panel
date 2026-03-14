add_library(can2040_driver STATIC
    ${CMAKE_SOURCE_DIR}/can2040/src/can2040.c
)

target_include_directories(can2040_driver PUBLIC
    ${CMAKE_SOURCE_DIR}/can2040/src
)

target_link_libraries(can2040_driver PUBLIC
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_clocks
    hardware_sync
)

add_library(scp_bsp_pico STATIC
    src/can_bus.c
)

target_include_directories(scp_bsp_pico PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(scp_bsp_pico PUBLIC
    can2040_driver
)
