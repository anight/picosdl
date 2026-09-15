#
# Locate the Pico SDK and pull in its CMake support.
#
# Shared by picosdl's own standalone build and by any game that embeds picosdl
# as a subdirectory, so there is one copy of this and not one per project. It
# deliberately does *not* choose PICO_BOARD: which board is being targeted is
# the application's decision, and the caller must set it before including this.
#
# Include it once, before project(), and call pico_sdk_init() after project().
#

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

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
