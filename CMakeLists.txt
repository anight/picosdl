cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

# Pico 2 W (RP2350). Override with -DPICO_BOARD=pico_w for the original board.
# The system clock stays at 128 MHz: that is what the ST7789 PIO timings were
# measured at, and it keeps the I2S divider honest. The display driver still
# pokes PIO registers directly; gpio_set_function() is used for the mux so the
# RP2350 pad-isolation latch is cleared.
set(PICO_BOARD pico2_w CACHE STRING "Board type")

if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "${USERHOME}/.pico-sdk/sdk/${sdkVersion}")
endif()
if(NOT PICO_TOOLCHAIN_PATH)
    set(PICO_TOOLCHAIN_PATH "${USERHOME}/.pico-sdk/toolchain/${toolchainVersion}")
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR "Pico SDK not found at '${PICO_SDK_PATH}' - set PICO_SDK_PATH")
endif()

include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(picosdl C CXX ASM)

pico_sdk_init()

if (NOT TARGET pico_btstack_cyw43)
    message(FATAL_ERROR
        "No Bluetooth support for PICO_BOARD='${PICO_BOARD}'. Use pico_w or pico2_w.")
endif()

# The two sibling projects this builds on. Their sources are referenced in place
# rather than copied, so fixes there do not have to be mirrored here.
set(PICOPOP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(LCD_DRIVER_DIR ${PICOPOP_ROOT}/pico-test-st7789/Raspberry-Pi-Pico-PIO-LCD-Driver)
set(BT_KEYBOARD_DIR ${PICOPOP_ROOT}/pico-test-bt-keyboard/src)

foreach(dir ${LCD_DRIVER_DIR} ${BT_KEYBOARD_DIR})
    if(NOT EXISTS ${dir})
        message(FATAL_ERROR "Expected to find ${dir} - is the picopop tree complete?")
    endif()
endforeach()

# ----------------------------------------------------------------- picosdl

add_library(picosdl STATIC
    src/psdl_init.c
    src/psdl_surface.c
    src/psdl_blit.c
    src/psdl_palette.c
    src/psdl_video.c
    src/psdl_events.c
    src/psdl_timer.c
    src/psdl_audio.c
    src/psdl_rwops.c
    src/psdl_stubs.c

    backend/pico/psdl_pico_video.c
    backend/pico/psdl_pico_audio.c
    backend/pico/psdl_pico_input.c
    backend/pico/psdl_pico_time.c
    backend/pico/pio-i2s.c
    backend/pico/joystick.c

    ${LCD_DRIVER_DIR}/dispPioSt7789.c

    ${BT_KEYBOARD_DIR}/bt_app.c
    ${BT_KEYBOARD_DIR}/bt_le.c
    ${BT_KEYBOARD_DIR}/bt_classic.c
    ${BT_KEYBOARD_DIR}/hid_report.c
    ${BT_KEYBOARD_DIR}/kbd_decode.c
    ${BT_KEYBOARD_DIR}/debug.c
)

target_include_directories(picosdl PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/backend/pico
    # btstack_config.h lives here, and it has to be PUBLIC: the pico_btstack_*
    # libraries are INTERFACE targets, so BTstack's own sources are compiled
    # into whatever executable links this - and every one of them includes it.
    ${BT_KEYBOARD_DIR}
)
target_include_directories(picosdl PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${LCD_DRIVER_DIR}
)

pico_generate_pio_header(picosdl ${CMAKE_CURRENT_LIST_DIR}/backend/pico/pio-i2s-out.pio)
pico_btstack_make_gatt_header(picosdl PRIVATE ${BT_KEYBOARD_DIR}/kbd_host.gatt)

target_link_libraries(picosdl PUBLIC
    pico_stdlib
    pico_rand
    pico_multicore
    hardware_adc
    hardware_dma
    hardware_pio
    pico_btstack_ble
    pico_btstack_classic
    pico_btstack_cyw43
    pico_cyw43_arch_threadsafe_background
)

target_compile_definitions(picosdl PUBLIC
    CYW43_LWIP=0                                  # Bluetooth only, no networking
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)

# Strict warnings for our own code only - the vendored drivers and the SDK are
# not warning-clean and compile into the same target.
set_source_files_properties(
    src/psdl_init.c src/psdl_surface.c src/psdl_blit.c src/psdl_palette.c
    src/psdl_video.c src/psdl_events.c src/psdl_timer.c src/psdl_audio.c
    src/psdl_rwops.c src/psdl_stubs.c
    backend/pico/psdl_pico_video.c backend/pico/psdl_pico_audio.c
    backend/pico/psdl_pico_input.c backend/pico/psdl_pico_time.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
)

# -------------------------------------------------------------------- demo

set(SDLPOP_DIR ${PICOPOP_ROOT}/SDLPoP/src)

add_executable(picosdl-demo
    demo/main.c
    demo/font5x7.c
    demo/game_glue.c

    # The game's own Adlib music player, used unmodified, reading the tunes and
    # the instrument bank straight out of the generated resources.
    ${SDLPOP_DIR}/midi.c
    ${SDLPOP_DIR}/resources.c
)

# Which OPL emulator midi.c drives.
#
#   dbopl  DOSBox's, the default. Measured on the title theme at 938
#          instructions per output sample against Nuked's 5583 - about 6x
#          cheaper - and 5176 bytes of chip state against Nuked's 20776. It is
#          the less exact chip model: level-matched to 0.06 dB with a 0.9995
#          loudness-envelope correlation, but ~1.3 dB brighter above 6 kHz.
#   nuked  Nuked OPL3 1.7.4, what SDLPoP ships, with picopop's silent-slot and
#          unfolded-table optimisations. More faithful, and too expensive for an
#          RP2040 at ~123M instructions/second.
#
# See PLAN.md section 8.
set(PICOPOP_OPL dbopl CACHE STRING "OPL emulator: dbopl or nuked")
set_property(CACHE PICOPOP_OPL PROPERTY STRINGS dbopl nuked)

if (PICOPOP_OPL STREQUAL "dbopl")
    target_sources(picosdl-demo PRIVATE
        ${SDLPOP_DIR}/dbopl.cpp
        ${SDLPOP_DIR}/dbopl_adapter.cpp)
    # opl3.h switches on this: midi.c keeps including it, but gets a token
    # opl3_chip instead of Nuked's 19 KB struct, and opl3.c compiles to nothing.
    target_compile_definitions(picosdl-demo PRIVATE PICOPOP_USE_DBOPL=1)
    # Vendored C++ with no runtime needs: no exceptions, no RTTI, no libstdc++.
    set_source_files_properties(${SDLPOP_DIR}/dbopl.cpp ${SDLPOP_DIR}/dbopl_adapter.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti;-fno-threadsafe-statics")
elseif (PICOPOP_OPL STREQUAL "nuked")
    target_sources(picosdl-demo PRIVATE ${SDLPOP_DIR}/opl3.c)
    target_compile_definitions(picosdl-demo PRIVATE PICOPOP_USE_DBOPL=0)
else()
    message(FATAL_ERROR "PICOPOP_OPL must be dbopl or nuked, not '${PICOPOP_OPL}'")
endif()
message(STATUS "OPL emulator: ${PICOPOP_OPL}")

target_link_libraries(picosdl-demo PRIVATE picosdl)
target_include_directories(picosdl-demo PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${SDLPOP_DIR}
)

# midi.c uses log2f/powf.
target_link_libraries(picosdl-demo PRIVATE pico_float)

# Our own code only; SDLPoP's is not warning-clean under -Wextra.
set_source_files_properties(demo/main.c demo/font5x7.c demo/game_glue.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# parse_midi() is the one thing in this firmware that uses the heap: it callocs
# a track array, then reallocs the event array once per event. The title theme
# is 436 events (~7 KB at 16 bytes each); the largest tune in the game is
# several times that.
#
# 20 KB was the Pico W ceiling (32 KB overflowed RAM by 2228 bytes). The Pico 2 W
# has 520 KB, so this is no longer load-bearing; it is kept as a measured bound
# for parse_midi() until the tunes are pre-parsed at build time (PLAN.md 3.3).
# The demo reports its peak heap use so the number is measured rather than assumed.
target_compile_definitions(picosdl-demo PRIVATE PICO_HEAP_SIZE=20480)

# Console on both UART0 (GP0/GP1, what the debugprobe bridges) and USB CDC, so
# either cable works.
pico_enable_stdio_uart(picosdl-demo 1)
pico_enable_stdio_usb(picosdl-demo 1)

pico_add_extra_outputs(picosdl-demo)
