cmake_minimum_required(VERSION 3.16)
project(ipc-data-relay VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------
option(IPCRELAY_BUILD_TESTS "Build unit and integration tests" ON)
option(IPCRELAY_BUILD_TOOLS "Build the test publisher and control tool" ON)
option(IPCRELAY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(ZeroMQ REQUIRED)
find_package(Threads REQUIRED)

include(GNUInstallDirs)

add_library(ipcrelay_warnings INTERFACE)
target_compile_options(ipcrelay_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wcast-align -Wformat=2)
if(IPCRELAY_WARNINGS_AS_ERRORS)
  target_compile_options(ipcrelay_warnings INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# Common library: wire protocol, capture format, config, UDP, logging
# ---------------------------------------------------------------------------
add_library(ipcrelay_common STATIC
  src/common/byteorder.cpp
  src/common/capture_format.cpp
  src/common/capture_reader.cpp
  src/common/capture_writer.cpp
  src/common/config.cpp
  src/common/json_writer.cpp
  src/common/log.cpp
  src/common/net_util.cpp
  src/common/reassembler.cpp
  src/common/sequence_tracker.cpp
  src/common/signal_handler.cpp
  src/common/time_util.cpp
  src/common/udp_multicast.cpp
  src/common/wire_protocol.cpp
  src/common/zmq_util.cpp)
target_include_directories(ipcrelay_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ipcrelay_common PUBLIC ZeroMQ::ZeroMQ Threads::Threads
  PRIVATE ipcrelay_warnings)
target_compile_definitions(ipcrelay_common PUBLIC
  IPCRELAY_VERSION_STRING="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Bridge: ZeroMQ IPC SUB -> UDP multicast
# ---------------------------------------------------------------------------
add_executable(ipc-relay-bridge
  src/bridge/main.cpp
  src/bridge/bridge.cpp
  src/bridge/bridge_config.cpp)
target_link_libraries(ipc-relay-bridge PRIVATE ipcrelay_common ipcrelay_warnings)

# ---------------------------------------------------------------------------
# Receiver: UDP multicast -> binary capture (+ ZeroMQ TCP stats/command)
# ---------------------------------------------------------------------------
add_executable(ipc-relay-receiver
  src/receiver/main.cpp
  src/receiver/receiver.cpp
  src/receiver/receiver_config.cpp
  src/receiver/receiver_stats.cpp)
target_link_libraries(ipc-relay-receiver PRIVATE ipcrelay_common ipcrelay_warnings)

install(TARGETS ipc-relay-bridge ipc-relay-receiver
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/ipc-data-relay
  FILES_MATCHING PATTERN "*.conf")
install(FILES tools/capture_inspect.py
  DESTINATION ${CMAKE_INSTALL_BINDIR}
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
  RENAME ipc-relay-capture-inspect)

# ---------------------------------------------------------------------------
# Tools: test publisher and runtime control utility
# ---------------------------------------------------------------------------
if(IPCRELAY_BUILD_TOOLS)
  add_executable(ipc-relay-testpub src/testpub/main.cpp)
  target_link_libraries(ipc-relay-testpub PRIVATE ipcrelay_common ipcrelay_warnings)

  add_executable(ipc-relay-ctl src/ctl/main.cpp)
  target_link_libraries(ipc-relay-ctl PRIVATE ipcrelay_common ipcrelay_warnings)

  install(TARGETS ipc-relay-testpub ipc-relay-ctl
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(IPCRELAY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
