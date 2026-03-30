cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2040_3d_vision_emitter C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(PIO_RP2040_TOOLS "$ENV{USERPROFILE}/.platformio/packages/tool-rp2040tools")
set(ELF2UF2_EXE "${PIO_RP2040_TOOLS}/elf2uf2.exe")

add_executable(rp2040_3d_vision_emitter
    src/main.c
    src/emitter.c
    src/ir_emitter.c
    src/status_led.c
    src/usb_descriptors.c
    src/usb_emitter_class.c
)

set_target_properties(rp2040_3d_vision_emitter PROPERTIES
    OUTPUT_NAME "RP2040_3D_Vision_Emitter"
)

target_include_directories(rp2040_3d_vision_emitter PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(rp2040_3d_vision_emitter PRIVATE
    pico_stdlib
    pico_multicore
    pico_time
    hardware_irq
    hardware_timer
    hardware_gpio
    hardware_pio
    hardware_dma
    hardware_clocks
    tinyusb_device
)

pico_enable_stdio_usb(rp2040_3d_vision_emitter 0)
pico_enable_stdio_uart(rp2040_3d_vision_emitter 1)

if (EXISTS "${ELF2UF2_EXE}")
    add_custom_command(TARGET rp2040_3d_vision_emitter POST_BUILD
        COMMAND "${ELF2UF2_EXE}" "$<TARGET_FILE:rp2040_3d_vision_emitter>" "$<TARGET_FILE_DIR:rp2040_3d_vision_emitter>/RP2040_3D_Vision_Emitter.uf2"
        VERBATIM
    )
endif()
