cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1-remote)

# Provides sample_usbd_init_device() and friends. Required: with
# CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n nothing brings USB up on its
# own, so without this the diagnostic console never enumerates. The looper
# does the same at its firmware/CMakeLists.txt:4.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

target_sources(app PRIVATE
  src/main.c
  src/board_io.c
  src/controls.c
  src/buttons.c
  src/profile.c
  src/txqueue.c
  src/presets.c
  src/presets_flash.c
  src/midi_tx.c
  src/midi_trs.c
  src/midi_usb.c
  src/config_console.c
)
