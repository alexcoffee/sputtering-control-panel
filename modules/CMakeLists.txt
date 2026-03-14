function(scp_add_module module_dir target_name)
    file(GLOB module_sources CONFIGURE_DEPENDS
        ${module_dir}/src/*.c
    )

    add_executable(${target_name}
        ${module_sources}
    )

    add_dependencies(${target_name} check_module_pins)

    target_include_directories(${target_name} PRIVATE
        ${module_dir}/include
    )

    target_link_libraries(${target_name}
        scp_shared
        pico_stdlib
    )

    if(target_name STREQUAL "scp_pirani" OR target_name STREQUAL "scp_ion_gauge")
        target_sources(${target_name} PRIVATE
            ${CMAKE_SOURCE_DIR}/lv_drivers/display/GC9A01.c
        )

        target_include_directories(${target_name} PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/lv_drivers
        )

        target_link_libraries(${target_name}
            lvgl
            hardware_adc
            hardware_spi
            hardware_pwm
        )

        pico_set_binary_type(${target_name} default)
    endif()

    pico_enable_stdio_uart(${target_name} 0)
    pico_enable_stdio_usb(${target_name} 1)
    pico_add_extra_outputs(${target_name})
endfunction()

set(SCP_ALL_MODULES
    pirani
    ion_gauge
    roughing_pump
)

if(NOT DEFINED SCP_BUILD_ALL_MODULES)
    set(SCP_BUILD_ALL_MODULES OFF)
endif()

if(SCP_BUILD_ALL_MODULES)
    foreach(module_name IN LISTS SCP_ALL_MODULES)
        scp_add_module(${CMAKE_CURRENT_SOURCE_DIR}/${module_name} scp_${module_name})
    endforeach()
else()
    list(FIND SCP_ALL_MODULES ${SCP_MODULE_TARGET} module_idx)
    if(module_idx EQUAL -1)
        message(FATAL_ERROR "SCP_MODULE_TARGET must be one of: ${SCP_ALL_MODULES}")
    endif()

    scp_add_module(${CMAKE_CURRENT_SOURCE_DIR}/${SCP_MODULE_TARGET} scp_${SCP_MODULE_TARGET})
endif()
