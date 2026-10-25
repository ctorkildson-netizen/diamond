# Diamond -- a word processor for the PicoCalc, after WordStar.
cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)
set(PICO_BOARD pico2_w CACHE STRING "Board type")
include(pico_sdk_import.cmake)
project(diamond C CXX ASM)
pico_sdk_init()

set(MODULES ${CMAKE_CURRENT_LIST_DIR}/modules)
set(DRV ${MODULES}/picocalc-text-starter/drivers)
include(${MODULES}/picocalc-extras/picker/picker.cmake)

add_executable(diamond
    src/main.c src/editor.c src/reader.c src/rfonts.c
    ${DRV}/fat32.c ${DRV}/sdcard.c ${DRV}/font-5x10.c ${DRV}/font-8x10.c
    ${DRV}/keyboard.c ${DRV}/lcd.c ${DRV}/southbridge.c
)
target_include_directories(diamond PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${MODULES}/picocalc-text-starter
    ${DRV}
    ${MODULES}/picocalc-shell/include)
target_compile_definitions(diamond PRIVATE PICO_MALLOC_PANIC=0)
pico_set_program_name(diamond "diamond")
pico_enable_stdio_uart(diamond 0)
pico_enable_stdio_usb(diamond 0)
target_link_libraries(diamond picocalc_picker
    pico_stdlib hardware_gpio hardware_i2c hardware_spi hardware_pio hardware_watchdog
    hardware_powman)
pico_add_extra_outputs(diamond)
