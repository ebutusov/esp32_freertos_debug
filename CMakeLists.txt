# Top-level project CMakeLists.txt
#
# Every ESP-IDF project needs exactly these three lines (in this order).
# - cmake_minimum_required: IDF's build system needs a recent CMake.
# - include(project.cmake): pulls in the whole IDF build system. It must come
#   BEFORE project(), because it overrides CMake's project() function with an
#   IDF-aware version that discovers components, runs Kconfig, etc.
# - project(): the name becomes the ELF/BIN name: build/freertos_debug_demo.elf
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Keep the dependency graph small: we only need what the "main" component
# REQUIRES (see main/CMakeLists.txt). This speeds up builds noticeably.
idf_build_set_property(MINIMAL_BUILD ON)

project(freertos_debug_demo)
