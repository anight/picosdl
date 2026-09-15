cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


#
# picosdl can be built two ways, and both have to work:
#
#   * standalone, as its own project - `cmake -S picosdl -B build` - which is
#     how its demo and its tests are built, and how another game would start;
#   * as a subdirectory of a game that has already initialised the Pico SDK,
#     which is what picopop's src/CMakeLists.txt does.
#
# Only the first may call project()/pico_sdk_init(); calling them twice is an
# error. PICOSDL_STANDALONE records which mode we are in.
#
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(PICOSDL_STANDALONE TRUE)

    # Pico 2 W (RP2350) by default. The system clock stays at 128 MHz: that is
    # what the ST7789 PIO timings were measured at, and it keeps the I2S
    # divider exact at 8 kHz. Override with -DPICO_BOARD=pico_w.
    set(PICO_BOARD pico2_w CACHE STRING "Board type")

    # Speed over size: -O3. This is the SDK's default anyway, but it is stated
    # here because the audio path is hard real-time and -Os is not fast enough
    # for it - a client's OPL synthesis missed the mixer's block deadline at -Os,
    # and a missed deadline is audible (the DMA chain re-triggers on stale buffer
    # contents; see the note in picodev.sh). Only set standalone: as a subdirectory
    # the parent project's choice wins, which is what a library should do.
    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING
            "Build type (Release = -O3; -Os is too slow for the mixer)" FORCE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            Debug Release MinSizeRel RelWithDebInfo)
    endif()

    include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_bootstrap.cmake)
    project(picosdl C CXX ASM)
    pico_sdk_init()
else()
    set(PICOSDL_STANDALONE FALSE)
endif()

if (NOT TARGET pico_btstack_cyw43)
    message(FATAL_ERROR
        "No Bluetooth support for PICO_BOARD='${PICO_BOARD}'. Use pico_w or pico2_w.")
endif()

# The hardware drivers live inside picosdl now, so the library is self-contained
# and carries no dependency on any particular game.
#
#   vendor/pio-st7789   the ST7789 PIO driver, a submodule of its own repo
#   backend/pico/bt     the Bluetooth HID keyboard, vendored source
#
set(LCD_DRIVER_DIR ${CMAKE_CURRENT_LIST_DIR}/vendor/pio-st7789)
set(BT_KEYBOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/backend/pico/bt)

if(NOT EXISTS ${LCD_DRIVER_DIR}/dispPioSt7789.c)
    message(FATAL_ERROR
        "vendor/pio-st7789 is empty - run: git submodule update --init --recursive")
endif()

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
    backend/pico/psdl_pico_log.c
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
    backend/pico/psdl_pico_log.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra"
)

# ------------------------------------------------------------------- the demo
#
# The library's example program, and the thing to run first on a new board. It
# exercises a palette animation driven by CLUT writes alone, a flash-resident
# sprite blitted plain, mirrored and XORed, text through the keyed blitter and the
# LIFO arena, a Bluetooth keyboard, an analog stick, a four-voice synth in an audio
# callback, and an original tune - with a live mixer-load and memory report on
# screen.
#
# It links picosdl and nothing else. It used to live in a game's tree and play that
# game's music, which meant the library had no example anyone else could build; the
# tune in demo/tune.c is original and the demo needs no data but its own.
#
# On by default standalone, off when picosdl is a subdirectory, because a game that
# pulls this in for its own build probably does not want a second binary. A client
# that does want it - for bring-up - sets PICOSDL_BUILD_DEMO before
# add_subdirectory().
#
option(PICOSDL_BUILD_DEMO "Build picosdl-demo" ${PICOSDL_STANDALONE})

if (PICOSDL_BUILD_DEMO)
    add_executable(picosdl-demo
        demo/main.c demo/font5x7.c demo/tune.c
    )
    target_link_libraries(picosdl-demo PRIVATE picosdl)
    target_include_directories(picosdl-demo PRIVATE ${CMAKE_CURRENT_LIST_DIR}/demo)

    set_source_files_properties(
        demo/main.c demo/font5x7.c demo/tune.c
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

    # Nothing in the demo allocates; the heap is here only for newlib's stdio
    # buffers.
    target_compile_definitions(picosdl-demo PRIVATE PICO_HEAP_SIZE=4096)

    # Land next to the top-level build directory either way, so the path to flash
    # is build/picosdl-demo.elf whether this is its own project or a subdirectory.
    set_target_properties(picosdl-demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

    pico_enable_stdio_uart(picosdl-demo 1)
    pico_enable_stdio_usb(picosdl-demo 1)
    pico_add_extra_outputs(picosdl-demo)
endif()
