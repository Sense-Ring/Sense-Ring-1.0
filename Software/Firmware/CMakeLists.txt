cmake_minimum_required(VERSION 3.20.0)

# Directs Zephyr to search your current application directory for the "boards" folder
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Links this out-of-tree application directly to the Zephyr kernel framework
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sensering_v29_firmware)

# Source file compilation mapping
target_sources(app PRIVATE src/main.c src/ppg.c src/battery.c src/vitals.c src/flash_store.c src/ble.c src/imu.c src/wallclock.c)