# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Board / platform ------------------------------------------------------
# The Fruit Jam is an RP2350B (QFN-80). PICO_BOARD selects the right GPIO count
# and flash size; adafruit_fruit_jam ships with recent Pico SDK releases.
set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Target board")
set(PICO_PLATFORM rp2350-arm-s     CACHE STRING "Target platform")

include(pico_sdk_import.cmake)

project(Adafruit_ColecoJam C CXX ASM)

pico_sdk_init()

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.1.0")
    message(FATAL_ERROR
        "Pico SDK 2.1.0 or newer is required (HSTX + RP2350 PIO-USB support).")
endif()

# --- Third-party sources ---------------------------------------------------
set(FATFS_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/fatfs)
if(NOT EXISTS ${FATFS_DIR}/ff.c)
    message(FATAL_ERROR
        "FatFs not found. Run tools/fetch_deps.sh, or unpack ELM-ChaN FatFs "
        "R0.15 into third_party/fatfs/.")
endif()

set(PIO_USB_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/Pico-PIO-USB)
if(NOT EXISTS ${PIO_USB_DIR}/src/pio_usb.c)
    message(FATAL_ERROR
        "Pico-PIO-USB not found. Run tools/fetch_deps.sh.")
endif()

# --- Executable ------------------------------------------------------------
add_executable(colecojam
    src/main.cpp

    src/emu/z80.cpp
    src/emu/tms9918.cpp
    src/emu/sn76489.cpp
    src/emu/coleco.cpp

    src/hw/video_hstx.cpp
    src/hw/video_hdmi.cpp
    src/hw/audio.cpp
    src/hw/sdcard.cpp
    src/hw/usb_host.cpp
    src/hw/usb_msc.cpp
    src/hw/usb_descriptors.cpp
    src/hw/cart_reader.cpp
    src/hw/bootloader.c

    src/ui/menu.cpp

    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffunicode.c
)

pico_generate_pio_header(colecojam
    ${CMAKE_CURRENT_LIST_DIR}/src/hw/audio_i2s.pio)

target_include_directories(colecojam PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/emu
    ${CMAKE_CURRENT_LIST_DIR}/src/hw
    ${CMAKE_CURRENT_LIST_DIR}/src/ui
    ${FATFS_DIR}
)

target_compile_definitions(colecojam PRIVATE
    # Required whenever Pico-PIO-USB is driven by TinyUSB rather than used
    # standalone. It switches pio_usb over to TinyUSB's memory and interrupt
    # ownership; without it tuh_init() can hang outright.
    PIO_USB_USE_TINYUSB=1
    PICO_DEFAULT_UART_TX_PIN=8
    PICO_DEFAULT_UART_RX_PIN=9
    PICO_STDIO_USB=0                 # USB is used for MSC, not a console
    PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
    # src/hw/bootloader.c emits the binary_info program-name record itself, in
    # flash, because the SDK's own copy ends up in RAM under copy_to_ram and the
    # pico-bootLoader then cannot read it. Suppress the SDK's so there is exactly
    # one such record.
    PICO_NO_BI_PROGRAM_NAME=1
)

# The name the pico-bootLoader matches against its /emu/emulators.txt allow-list
# (see src/hw/bootloader.c). PICO_TARGET_NAME would already yield "colecojam",
# but that is an implicit fallback; say it out loud, and give bootloader.c a
# PICO_PROGRAM_NAME to build its record from.
pico_set_program_name(colecojam "colecojam")

target_compile_options(colecojam PRIVATE
    -O3
    -Wall
    -Wno-unused-function
    -ffunction-sections
    -fdata-sections
)

target_link_libraries(colecojam
    pico_stdlib
    pico_multicore
    pico_unique_id
    pico_bootrom
    hardware_dma
    hardware_pio
    hardware_spi
    hardware_i2c
    hardware_irq
    hardware_clocks
    hardware_vreg
    hardware_watchdog
    tinyusb_device
    tinyusb_board
)

# tinyusb_host vs tinyusb_host_base: the former drags in the native RP2040 host
# controller driver, which is wrong for a build where the native port is the
# MSC *device* and the host lives on PIO-USB. tinyusb_host_base omits the
# native HCD and is what working Fruit Jam projects link.
if(TARGET tinyusb_host_base)
    target_link_libraries(colecojam tinyusb_host_base tinyusb_common)
else()
    message(WARNING "tinyusb_host_base not found; falling back to tinyusb_host")
    target_link_libraries(colecojam tinyusb_host)
endif()

# HSTX is register-header-only in the Pico SDK -- hardware/structs/hstx_ctrl.h
# and hstx_fifo.h arrive with pico_stdlib, and there is no hardware_hstx
# library to link. Naming one anyway makes CMake pass it through to the linker
# as -lhardware_hstx, which fails. Link it only if a future SDK adds the target.
if(TARGET hardware_hstx)
    target_link_libraries(colecojam hardware_hstx)
endif()

# --- pico_hdmi -------------------------------------------------------------
# Optional: only needed for VIDEO_DRIVER_PICO_HDMI, which is what carries sound
# over HDMI. Fetch with tools/fetch_deps.sh, or unpack the repository into
# third_party/pico_hdmi.
# Mirrors VIDEO_DRIVER in include/config.h. CMake cannot read that header, so
# the two must be kept in step by hand: ON here means VIDEO_DRIVER_PICO_HDMI
# there. A mismatch is caught at compile time by the static_assert in
# video_hdmi.cpp, which only builds under the library driver.
option(VIDEO_DRIVER_IS_PICO_HDMI
       "Build for VIDEO_DRIVER_PICO_HDMI (HDMI audio, run from RAM)" ON)

set(PICO_HDMI_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/pico_hdmi)
if(EXISTS ${PICO_HDMI_DIR}/CMakeLists.txt)
    # MUST be set before add_subdirectory, and it MUST be set here rather than
    # in config.h -- the library never includes our headers. It reads this as a
    # CMake cache variable and turns it into MODE_HSTX_CLK_DIV.
    #
    # The library computes clk_hstx = clk_sys / MODE_HSTX_CLK_DIV. We run
    # clk_sys at 252 MHz because Pico-PIO-USB needs a multiple of 12 MHz, and
    # 640x480p60 needs clk_hstx = 126 MHz, so the divider is 2. Left at its
    # default of 1 the library drives HSTX at 252 MHz -- double spec, no
    # picture, and video_output_init() does not come back.
    set(PICO_HDMI_HSTX_CLK_DIV 2 CACHE STRING "HSTX clock divider" FORCE)

    add_subdirectory(${PICO_HDMI_DIR} pico_hdmi)
    target_link_libraries(colecojam pico_hdmi)
    target_include_directories(colecojam PRIVATE ${PICO_HDMI_DIR}/include)
    message(STATUS "colecojam: pico_hdmi found -- HDMI audio available")
else()
    message(STATUS
        "colecojam: pico_hdmi not present. VIDEO_DRIVER_PICO_HDMI (and with it "
        "audio over HDMI) will not build. Run tools/fetch_deps.sh.")
endif()

# --- Pico-PIO-USB ----------------------------------------------------------
# Build it through upstream's own CMakeLists rather than hand-listing the
# sources. Its build sets compile definitions and PIO header generation that a
# hand-rolled add_library() silently omits -- and missing those is enough to
# hang tuh_init().
add_subdirectory(${PIO_USB_DIR} pico_pio_usb)

target_link_libraries(colecojam pico_pio_usb)

# Silence -Wattributes on Pico-PIO-USB's own sources.
#
# usb_crc.h declares update_usb_crc16() as `static inline` and then wraps it in
# __time_critical_func(), which adds noinline so the code stays resident in RAM
# rather than being paged from flash. GCC warns about seeing inline and
# noinline on one function. The noinline wins, which is what upstream intends,
# so the warning is noise -- but it fires on every file that includes the
# header, which buries warnings that do matter.
#
# Scoped to these four files rather than the whole target, and applied here
# rather than by editing third_party/ (which fetch_deps.sh would overwrite).
set_source_files_properties(
    ${PIO_USB_DIR}/src/pio_usb.c
    ${PIO_USB_DIR}/src/pio_usb_host.c
    ${PIO_USB_DIR}/src/pio_usb_device.c
    ${PIO_USB_DIR}/src/usb_crc.c
    PROPERTIES COMPILE_OPTIONS "-Wno-attributes")

# --- TinyUSB's PIO-USB host controller driver ------------------------------
# Setting CFG_TUH_RPI_PIO_USB=1 makes TinyUSB's native hcd_rp2040.c compile
# itself out, on the assumption that hcd_pio_usb.c takes its place. But the SDK
# only adds hcd_pio_usb.c to the build through its own tinyusb_pico_pio_usb
# target, which in turn needs the Pico-PIO-USB submodule inside lib/tinyusb.
# We ship our own copy of Pico-PIO-USB instead, so pull the HCD in directly --
# otherwise usbh.c links against hcd_init, hcd_edpt_xfer and friends and finds
# nothing at all.
if(NOT DEFINED PICO_TINYUSB_PATH)
    set(PICO_TINYUSB_PATH ${PICO_SDK_PATH}/lib/tinyusb)
endif()
set(TUSB_PIO_HCD ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c)

# Only add it if the SDK's own TinyUSB target does not already supply it --
# adding it twice gives duplicate symbols.
set(NEED_PIO_HCD TRUE)
if(TARGET tinyusb_host_base)
    get_target_property(_tuh_srcs tinyusb_host_base INTERFACE_SOURCES)
    if(_tuh_srcs)
        foreach(_src IN LISTS _tuh_srcs)
            if(_src MATCHES "hcd_pio_usb")
                set(NEED_PIO_HCD FALSE)
            endif()
        endforeach()
    endif()
endif()

if(NEED_PIO_HCD)
    if(NOT EXISTS ${TUSB_PIO_HCD})
        message(FATAL_ERROR
            "Could not find hcd_pio_usb.c at:\n  ${TUSB_PIO_HCD}\n"
            "Check that PICO_TINYUSB_PATH points at your pico-sdk/lib/tinyusb.")
    endif()
    target_sources(colecojam PRIVATE ${TUSB_PIO_HCD})
    message(STATUS "Adafruit_ColecoJam: adding TinyUSB PIO host driver")
endif()

# ffsystem.c supplies ff_memalloc/ff_memfree. With FF_USE_LFN 1 we no longer
# need them, but compile it when present so raising FF_USE_LFN back to 3 does
# not silently break the link. --gc-sections drops it if unused.
if(EXISTS ${FATFS_DIR}/ffsystem.c)
    target_sources(colecojam PRIVATE ${FATFS_DIR}/ffsystem.c)
endif()

# Keep the FatFs config in sync. fetch_deps.sh only copies ffconf.h on the
# first run, so edits to tools/ffconf.h would otherwise be ignored forever.
configure_file(${CMAKE_CURRENT_LIST_DIR}/tools/ffconf.h ${FATFS_DIR}/ffconf.h COPYONLY)

# Some SDK versions do compile PIO-USB sources inside tinyusb_host itself, and
# those need pio_usb.h on their include path. Our include directory above only
# propagates to targets that link pico_pio_usb, so push it onto tinyusb_host as
# well. Harmless when that target compiles nothing PIO-USB related.
if(TARGET tinyusb_host)
    get_target_property(_tuh_type tinyusb_host TYPE)
    if(_tuh_type STREQUAL "INTERFACE_LIBRARY")
        target_include_directories(tinyusb_host INTERFACE ${PIO_USB_DIR}/src)
    else()
        target_include_directories(tinyusb_host PUBLIC ${PIO_USB_DIR}/src)
    endif()
endif()

# --- Run from RAM when pico_hdmi drives the display ------------------------
# Both cores fetch instructions through the same XIP cache. pico_hdmi's core-1
# scanline loop has roughly a 6 us deadline per line, and when core 0 does
# heavy flash-resident work -- sd_init() and FatFs are the first such work in
# the boot sequence -- core 1 stalls on cache misses, misses the deadline, and
# the HSTX command stream desyncs. The picture dies while every other
# subsystem carries on reporting success.
#
# Copying the image to SRAM removes XIP from the picture entirely. It costs
# flash-to-RAM startup time and the text has to fit alongside the statics:
# ~90 KB text + ~298 KB bss = ~387 KB of the 520 KB available.
if(VIDEO_DRIVER_IS_PICO_HDMI)
    pico_set_binary_type(colecojam copy_to_ram)
    message(STATUS "colecojam: copy_to_ram enabled for pico_hdmi timing")
endif()

# --- pico-bootLoader variant -----------------------------------------------
# OFF gives the normal standalone image, linked at 0x10000000 and flashed over
# BOOTSEL. ON relinks into the resident bootloader's application partition at
# 0x10080000 so its picker can flash and launch us; see cmake/BootPartition.cmake
# for the flash map and for why the usual helper cannot be used verbatim here.
# Use ./fruitjam-build-forbootloader.sh rather than setting this by hand.
#
# Must come after pico_set_binary_type() above -- the relink picks its approach
# from the target's binary type -- and before pico_add_extra_outputs() below.
option(BUILD_FOR_BOOTLOADER "Relink for the pico-bootLoader app partition" OFF)
if(BUILD_FOR_BOOTLOADER)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/BootPartition.cmake)
    coleco_offset_for_bootloader(colecojam)
endif()

# --- Output ----------------------------------------------------------------
pico_add_extra_outputs(colecojam)     # produces .uf2, .elf, .bin, .hex

# Serial debug goes out the UART pins on the 2x16 header, since USB is busy.
pico_enable_stdio_usb(colecojam 0)
pico_enable_stdio_uart(colecojam 1)

# Copy the .uf2 to something recognisable on the RP2350 bootloader drive.
#
# Skipped for the bootloader variant: there the file that matters is the
# target's own colecojam.uf2, which is what pico-bootLoader's build_emulators.sh
# looks for and what has to be named colecojam.uf2 on the SD card. Copying to a
# fixed ${CMAKE_CURRENT_LIST_DIR}/build/ path would also have let the two build
# trees overwrite each other's output.
if(NOT BUILD_FOR_BOOTLOADER)
    add_custom_command(TARGET colecojam POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE_DIR:colecojam>/colecojam.uf2
            ${CMAKE_BINARY_DIR}/Adafruit_ColecoJam.uf2
        COMMENT "UF2 written to Adafruit_ColecoJam.uf2")
endif()
