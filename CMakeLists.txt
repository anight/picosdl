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

    # Pico 2 W (RP2350) by default. The system clock is 125 MHz - below the
    # SDK's 150 MHz default, so the core is not overclocked, and it puts the
    # panel's SCK at sysclk/2 = 62.5 MHz, exactly the ST7789 maximum, so neither
    # end is. It keeps the I2S divider exact at 8 kHz and within 12 ppm at 22050.
    # See the clock comment in backend/pico/board.h before changing it.
    #
    # All four Pico boards build: -DPICO_BOARD=pico, pico_w, pico2 or pico2_w.
    # The two without a radio simply default the Bluetooth keyboard off.
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

# The hardware drivers live inside picosdl, so the library is self-contained and
# carries no dependency on any particular game.
#
#   pio-st7789          the ST7789 PIO driver, a submodule of its own repo
#   pio-i2s             the I2S output driver, a submodule of its own repo
#   backend/pico/bt     the Bluetooth HID keyboard, vendored source
#
# Both are peers rather than vendored third parties: each is our own fork of an
# upstream project, tracked on its main branch and edited alongside this code.
# Neither knows anything about picosdl: they build against the Pico SDK and their
# own headers alone, and what they need from the board arrives at init.
# pio-st7789 is told its pin numbers at dispInit(); pio-i2s carries one change, a
# clock-divider check that upstream makes stricter than the hardware needs. They
# sit at the top level for the same reason picosdl and SDLPoP do in the project
# above, rather than under a vendor/ directory that would suggest they are
# someone else's code we merely bundle.
#
set(LCD_DRIVER_DIR ${CMAKE_CURRENT_LIST_DIR}/pio-st7789)
set(BT_KEYBOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/backend/pico/bt)

if(NOT EXISTS ${LCD_DRIVER_DIR}/dispPioSt7789.c)
    message(FATAL_ERROR
        "pio-st7789 is empty - run: git submodule update --init --recursive")
endif()

if(PICOSDL_AUDIO AND NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/pio-i2s/src/pio-i2s.c)
    message(FATAL_ERROR
        "pio-i2s is empty - run: git submodule update --init --recursive")
endif()

# ----------------------------------------------------------------- picosdl

# ------------------------------------------------------------------ the inputs
#
# Three independent input devices, each optional, and none of them required. A
# build with all three off is legitimate: it produces a library with a display and
# a speaker and no way to press anything, which is exactly what a kiosk or an
# attract-mode demo wants. Nothing here tries to talk you out of it.
#
# Off means *absent*, not ignored: the driver is not compiled, and in the Bluetooth
# case neither is BTstack nor the CYW43 firmware blob it carries.
#
# Bluetooth is the one input that depends on the board rather than on what is
# soldered to it: it needs the CYW43 radio, which the two `_w` boards have and the
# other two do not. So its default is what this board can actually do, which is
# what lets all four Pico boards configure and build with no arguments.
#
# Asking for it explicitly on a board without a radio is still an error. A silent
# downgrade would hand back a library with no keyboard and no indication of why,
# and the request is unambiguous enough to answer plainly.
#
if (TARGET pico_btstack_cyw43)
    set(PICOSDL_BT_DEFAULT ON)
else()
    set(PICOSDL_BT_DEFAULT OFF)
endif()

option(PICOSDL_INPUT_BT_KEYBOARD "Bluetooth HID keyboard (needs a CYW43 radio)" ${PICOSDL_BT_DEFAULT})

if (PICOSDL_INPUT_BT_KEYBOARD AND NOT TARGET pico_btstack_cyw43)
    message(FATAL_ERROR
        "PICOSDL_INPUT_BT_KEYBOARD is ON but PICO_BOARD='${PICO_BOARD}' has no "
        "CYW43 radio. Use pico_w or pico2_w, or set PICOSDL_INPUT_BT_KEYBOARD=OFF.")
endif()

option(PICOSDL_INPUT_JOYSTICK    "Analog joystick on the ADC" ON)
option(PICOSDL_INPUT_GAMEPAD     "I2C game controller (Adafruit Gamepad QT)" ON)

#
# Audio, on the same footing: optional, and a board without a MAX98357A and a
# speaker is a normal thing to have.
#
# Off is not "silent with the machinery still there" - the I2S driver and the PIO
# program are not built, and SDL_OpenAudio() fails with a clear error. That is the
# honest answer to "open the audio device" when there is no audio device, and it is
# what SDL itself does; both clients here already handle it, because opening audio
# is allowed to fail on a desktop too.
#
# Turning it off also frees PIO1, DMA channels 4 and 5, and core 1 - which does
# nothing else in this library but mix.
#
option(PICOSDL_AUDIO "I2S audio output (MAX98357A)" ON)

#
# Colour depth, and the one option that changes the shape of the API.
#
# 8 is picosdl as documented: indexed pixels, one global palette, the CLUT
# expanded by the PIO. Every blitter and surface in the library works in it.
#
# 16 is for a client that produces RGB565 itself - a 3D software rasteriser
# typically does, because it is what the panel wants. The indexed layer is then
# dropped rather than bridged: no window surface, no palette, no blitters, and no
# screen pool compiled in (128 KB back). The client presents its own buffer with
# PSDL_PresentRGB565() and keeps everything else picosdl does.
#
# A client is written for one or the other and finds out at compile time:
# SDL_CreateWindow() fails at 16, PSDL_PresentRGB565() is absent at 8.
#
set(PICOSDL_COLOR_DEPTH 8 CACHE STRING "Panel colour depth: 8 (indexed) or 16 (RGB565)")
set_property(CACHE PICOSDL_COLOR_DEPTH PROPERTY STRINGS 8 16)
if (NOT PICOSDL_COLOR_DEPTH EQUAL 8 AND NOT PICOSDL_COLOR_DEPTH EQUAL 16)
    message(FATAL_ERROR "PICOSDL_COLOR_DEPTH must be 8 or 16, got '${PICOSDL_COLOR_DEPTH}'")
endif()
message(STATUS "picosdl colour depth: ${PICOSDL_COLOR_DEPTH}bpp")

message(STATUS "picosdl inputs: BT keyboard=${PICOSDL_INPUT_BT_KEYBOARD} "
               "joystick=${PICOSDL_INPUT_JOYSTICK} gamepad=${PICOSDL_INPUT_GAMEPAD}")
message(STATUS "picosdl audio: ${PICOSDL_AUDIO}")

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
    src/psdl_gamecontroller.c
    src/psdl_font.c

    backend/pico/psdl_pico_video.c
    backend/pico/psdl_pico_input.c
    backend/pico/psdl_pico_time.c
    backend/pico/psdl_pico_cpu.c
    backend/pico/psdl_pico_log.c

    ${LCD_DRIVER_DIR}/dispPioSt7789.c
)

if (PICOSDL_AUDIO)
    target_sources(picosdl PRIVATE backend/pico/psdl_pico_audio.c)

    # pio-i2s brings its own library target, its own include directory and a
    # pre-generated PIO header (pio_version 0, so it assembles for both RP2040
    # and RP2350) - so there is nothing to add to picosdl's own sources and no
    # pico_generate_pio_header call. Its example and tests build only when it is
    # the top-level project, which it is not here.
    add_subdirectory(pio-i2s EXCLUDE_FROM_ALL)
    target_link_libraries(picosdl PUBLIC pio-i2s-lib)
    target_compile_definitions(picosdl PUBLIC PSDL_HAVE_AUDIO=1)
endif()



if (PICOSDL_INPUT_JOYSTICK)
    target_sources(picosdl PRIVATE backend/pico/joystick.c)
    target_compile_definitions(picosdl PUBLIC PSDL_HAVE_JOYSTICK=1)
endif()

if (PICOSDL_INPUT_GAMEPAD)
    target_sources(picosdl PRIVATE backend/pico/seesaw_gamepad.c)
    target_compile_definitions(picosdl PUBLIC PSDL_HAVE_GAMEPAD=1)
endif()

if (PICOSDL_INPUT_BT_KEYBOARD)
    target_sources(picosdl PRIVATE
        ${BT_KEYBOARD_DIR}/bt_app.c
        ${BT_KEYBOARD_DIR}/bt_le.c
        ${BT_KEYBOARD_DIR}/bt_classic.c
        ${BT_KEYBOARD_DIR}/hid_report.c
        ${BT_KEYBOARD_DIR}/kbd_decode.c
        ${BT_KEYBOARD_DIR}/debug.c
    )
    target_compile_definitions(picosdl PUBLIC PSDL_HAVE_BT_KEYBOARD=1)
endif()

target_compile_definitions(picosdl PUBLIC PSDL_COLOR_DEPTH=${PICOSDL_COLOR_DEPTH})

target_include_directories(picosdl PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/backend/pico
)
if (PICOSDL_INPUT_BT_KEYBOARD)
    # btstack_config.h lives here, and it has to be PUBLIC: the pico_btstack_*
    # libraries are INTERFACE targets, so BTstack's own sources are compiled
    # into whatever executable links this - and every one of them includes it.
    target_include_directories(picosdl PUBLIC ${BT_KEYBOARD_DIR})
endif()
target_include_directories(picosdl PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${LCD_DRIVER_DIR}
)

if (PICOSDL_INPUT_BT_KEYBOARD)
    pico_btstack_make_gatt_header(picosdl PRIVATE ${BT_KEYBOARD_DIR}/kbd_host.gatt)
endif()

target_link_libraries(picosdl PUBLIC
    pico_stdlib
    pico_rand
    pico_multicore
    hardware_dma
    hardware_pio
)

target_compile_definitions(picosdl PUBLIC
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)

if (PICOSDL_INPUT_JOYSTICK)
    target_link_libraries(picosdl PUBLIC hardware_adc)
endif()

if (PICOSDL_INPUT_GAMEPAD)
    target_link_libraries(picosdl PUBLIC hardware_i2c)
endif()

#
# Bluetooth is the expensive one, which is why turning it off is worth doing.
#
# It brings in BTstack twice over - the BLE and Classic stacks, because a keyboard
# may be either and you cannot tell from the outside - plus the CYW43 driver and,
# with it, that chip's firmware blob. The blob alone is 234 KB of flash and is not
# code, so nothing can shrink it.
#
# CYW43_LWIP=0 belongs here rather than above: it says "radio, but no networking",
# and without the radio there is nothing for it to say.
#
if (PICOSDL_INPUT_BT_KEYBOARD)
    target_link_libraries(picosdl PUBLIC
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        pico_cyw43_arch_threadsafe_background
    )
    target_compile_definitions(picosdl PUBLIC CYW43_LWIP=0)
endif()

# Strict warnings for our own code only - the drivers and the SDK are not
# warning-clean and compile into the same target.
set_source_files_properties(
    src/psdl_init.c src/psdl_surface.c src/psdl_blit.c src/psdl_palette.c
    src/psdl_video.c src/psdl_events.c src/psdl_timer.c src/psdl_audio.c
    src/psdl_rwops.c src/psdl_stubs.c src/psdl_gamecontroller.c
    src/psdl_font.c
    backend/pico/psdl_pico_video.c backend/pico/psdl_pico_audio.c
    backend/pico/psdl_pico_input.c backend/pico/psdl_pico_time.c
    backend/pico/psdl_pico_cpu.c backend/pico/psdl_pico_log.c
    # Listed whether or not the options above included them: a property on a file
    # that is not in the target is ignored, which is simpler than two lists.
    backend/pico/joystick.c backend/pico/seesaw_gamepad.c
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
#
# An 8bpp program: it animates the CLUT and blits indexed sprites, which at depth
# 16 would draw nonsense into an RGB565 buffer. So it is not built by default
# there. Ask for it explicitly if you want it anyway.
#
if (PICOSDL_STANDALONE AND PICOSDL_COLOR_DEPTH EQUAL 8)
    set(PICOSDL_BUILD_DEMO_DEFAULT ON)
else()
    set(PICOSDL_BUILD_DEMO_DEFAULT OFF)
endif()

option(PICOSDL_BUILD_DEMO "Build picosdl-demo (8bpp only)" ${PICOSDL_BUILD_DEMO_DEFAULT})

if (PICOSDL_BUILD_DEMO)
    add_executable(picosdl-demo
        demo/main.c demo/tune.c
    )
    target_link_libraries(picosdl-demo PRIVATE picosdl)
    target_include_directories(picosdl-demo PRIVATE ${CMAKE_CURRENT_LIST_DIR}/demo)

    set_source_files_properties(
        demo/main.c demo/tune.c
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
