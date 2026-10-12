# SPDX-License-Identifier: BSD-3-Clause
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
get_filename_component(_sdk "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_C_COMPILER "${_sdk}/bin/arm-uclinuxfdpiceabi-gcc")
set(CMAKE_CXX_COMPILER "${_sdk}/bin/arm-uclinuxfdpiceabi-g++")
set(CMAKE_AR "${_sdk}/bin/arm-uclinuxfdpiceabi-ar")
set(CMAKE_RANLIB "${_sdk}/bin/arm-uclinuxfdpiceabi-ranlib")
set(CMAKE_SYSROOT "${_sdk}/sysroot")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Runtime probes cannot execute ARM programs on the x86_64 build host.
# Projects needing try_run must provide cross results or an explicit emulator.
